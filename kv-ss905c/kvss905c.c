// Copyright 2007 Google Inc. All Rights Reserved.
// Author: agl@imperialviolet.org (Adam Langley)
//
// Copyright (C) 2007 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include <arpa/inet.h>

#include <scsi/sg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define REQUEST_SENSE_SIZE 20
#ifndef SG_FLAG_MMAP_IO
#define SG_FLAG_MMAP_IO 4
#endif

#define u8 uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t

#include "kvss905c.h"

static const unsigned kMaxBuffer = 0x10000;

// -----------------------------------------------------------------------------
// This is a series of utility functions for dealing with SCSI tape devices.
//
// See http://www.tldp.org/HOWTO/SCSI-Generic-HOWTO/index.html
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Dump the given requestsense buffer to stderr in a, hopefully, useful format.
// The requestsense buffer should result from a failed SCSI operation
// -----------------------------------------------------------------------------
void
scsi_request_sense_dump(void *irequestsense) {
  // see http://www.t10.org/lists/asc-num.htm
  const u8 *requestsense = (u8 *) irequestsense;
  fprintf(stderr, "Request failed: ");
  for (int i = 0; i < REQUEST_SENSE_SIZE; ++i) {
    fprintf(stderr, "%02x ", requestsense[i]);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "ASC/ASCQ: %02x %02x\n",
          requestsense[12], requestsense[13]);
}

// -----------------------------------------------------------------------------
// Returns non-zero if the given error is transient (e.g. due to the device not
// being ready
// -----------------------------------------------------------------------------
static int
scsi_transient_error(void *irequestsense) {
  // see http://www.t10.org/lists/asc-num.htm
  const u8 *requestsense = (u8 *) irequestsense;
  const u8 asc = requestsense[12];

  if (asc == 0x28 || asc == 0x29 || asc == 0x04) return 1;

  return 0;
}

// -----------------------------------------------------------------------------
// Returns the SCSI error code from a request-sense-buffer.
// See: http://www.t10.org/lists/asc-num.htm
// -----------------------------------------------------------------------------
u16
scsi_error_code(const u8 *requestsense) {
  // see http://www.t10.org/lists/asc-num.htm
  return (requestsense[12] << 8) | requestsense[13];
}

// -----------------------------------------------------------------------------
// Perform a SCSI command
//   fd: file descriptor of the tape device
//   direction: either SG_DXFER_TO_DEV or SG_DXFER_FROM_DEV
//   command: SCSI command bytes
//   command_length: length, in bytes, of command
//   data: payload data. If NULL, the mmap buffer is used
//   data_length: length of the payload data
//   requestsense: (output) resulting error buffer
//   timeout: timeout in milliseconds. Must be > 0
//
// Returns:
//   0 on success
//   1 in the case of an ioctl error
//   2 in the case of a SCSI error
// -----------------------------------------------------------------------------
static int
scsi_command(int fd, int direction,
             const void *command, unsigned command_length,
             void *data, unsigned data_length,
             void *requestsense, int timeout) {
  for (int retries = 0; retries < 5; ++retries) {
    sg_io_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.interface_id = 'S';

    if (!timeout) timeout = 30000;

    hdr.dxferp = data;
    hdr.dxfer_len = data_length;
    hdr.cmdp = (unsigned char *) command;
    hdr.cmd_len = command_length;
    if (!data) hdr.flags |= SG_FLAG_MMAP_IO;
    hdr.sbp = (unsigned char *) requestsense;
    hdr.timeout = timeout;
    if (requestsense) hdr.mx_sb_len = REQUEST_SENSE_SIZE;

    hdr.dxfer_direction = direction;

    if (ioctl(fd, SG_IO, &hdr)) {
      perror("SG_IO");
      return 1;
    }

    if (hdr.masked_status) {
      if (scsi_transient_error(requestsense)) {
        sleep(3);
        continue;
      }
      return 2;
    }

    return 0;
  }

  return 2;
}

// -----------------------------------------------------------------------------
// KVSS905C specific function. See the header file for comments...

void
kvss905c_window_init(struct kvss905c_window *window) {
  memset(window, 0, sizeof(struct kvss905c_window));

  window->composition = KVSS905C_COMPOSITION_COLOUR;
  window->bpp = 24;
  window->xres = window->yres = 300;  // 300 dpi
  window->number_of_pages_to_scan = 0xff;  // all pages
  window->emphasis = 0x2f;
  window->document_size = 7;  // US letter
  window->double_feed_detector = 1;
  window->subsample = 3;  // 4:2:2 JPEG subsampling

  // US letter sizes
  window->document_width = window->width = 8.5*1200;
  window->document_length = window->length = 11*1200;

  window->compression_type = 0x81;  // JPEG
  window->compression_argument = 85;  // quality 85
}

static int
kvss905c_window_serialise(u8 *output,
                          const struct kvss905c_window *window) {
  unsigned j = 0;
#define U8(x) output[j++] = window->x;
#define U16(x) do { u16 t = htons(window->x); memcpy(output + j, &t, 2); j += 2; } while (0);
#define U32(x) do { u32 t = htonl(window->x); memcpy(output + j, &t, 4); j += 4; } while (0);
#define U8I(x) output[j++] = x;

  U8I(0);
  U8I(0);
  U16(xres);
  U16(yres);
  U32(x1);
  U32(y1);
  U32(width);
  U32(length);
  U8(brighness);
  U8(threshold);
  U8(contrast);
  U8(composition);
  U8(bpp);
  U16(halftone_pattern);
  U8I(window->reverse_image ? 0x80 : 0);
  U16(bit_ordering);
  U8(compression_type);
  U8(compression_argument);
  j += 6;
  U8I(0);
  U8I(window->flatbed << 7 |
      window->stop_on_skew << 4 |
      window->disable_buffering << 3 |
      window->continue_on_double_feed << 0);
  U8(mirror_image);
  U8(emphasis);
  U8(gamma_correction);
  U8I(window->multi_colour_drop_out << 7 |
      window->lamp << 4 |
      window->double_feed_sensitivity << 0);
  U8I(window->remove_moire << 6 |
      window->subsample << 4 |
      window->colour_match << 0);
  U8(document_size);
  U32(document_width);
  U32(document_length);
  U8I(window->ahead_disable << 7 |
      window->deskew << 5 |
      window->double_feed_detector << 4 |
      window->full_size_scan << 2 |
      window->feed_slow << 1 |
      window->remove_shadow << 0);
  U8(number_of_pages_to_scan);
  U8(threshold_mode);
  U8(separation_mode);
  U8(standard_white_level);
  U8I(window->blackwhite_noise_reduction << 7 |
      window->noise_reduction << 0);
  U8I(window->manual_feed_mode << 6 |
      window->additional_space_top << 5 |
      window->additional_space_bottom << 4 |
      window->detect_separation_sheet << 3 |
      window->halt_at_separation_sheet << 2 |
      window->detect_control_sheet << 1);
  U8(stop_mode);
  //U8(red_chroma);
  //U8(blue_chroma);

#undef U8
#undef U16
#undef U32
#undef U8I

  return j;
}

#define WINDOW_SIZE 64

int
kvss905c_set_windows(int fd,
            const struct kvss905c_window *window,
            char duplex,
            u8 *requestsense) {
  // see page 35
  u8 windowbytes[6 + 2 + WINDOW_SIZE];
  memset(windowbytes, 0, sizeof(windowbytes));

  const int bytes_written = kvss905c_window_serialise(windowbytes + 8, window);
  if (bytes_written != WINDOW_SIZE) abort();

  const u16 length = htons(WINDOW_SIZE);
  memcpy(windowbytes + 6, &length, sizeof(length));
  static const int transfer_length = sizeof(windowbytes);

  const u8 command[] = {0x24, 0, 0, 0, 0, 0, transfer_length >> 16,
                        transfer_length >> 8, transfer_length, 0};
  const int r = scsi_command(fd, SG_DXFER_TO_DEV, command, sizeof(command),
                             windowbytes, transfer_length, requestsense, 0);

  if (r) return r;

  if (duplex) {
    windowbytes[8] = 0x80;
    return scsi_command(fd, SG_DXFER_TO_DEV, command, sizeof(command),
                        windowbytes, transfer_length, requestsense, 0);
  }

  return 0;
}

int
kvss905c_scan(int fd, u8 *requestsense) {
  // see page 33
  static const u8 command[] = {0x1b, 0, 0, 0, 0, 0};

  return scsi_command(fd, SG_DXFER_TO_DEV, command, sizeof(command), NULL, 0,
                      requestsense, 0);
}

int
get_data_buffer_status(int fd, u8 *window_id, u32 *length, u8 *requestsense) {
  // see page 71
  u8 buffer[12];
  static const u8 command[] = {0x34, 0, 0, 0, 0, 0, 0, 0, sizeof(buffer), 0};
  const int r = scsi_command(fd, SG_DXFER_FROM_DEV, command, sizeof(command),
                             buffer, sizeof(buffer), requestsense, 0);
  if (r) return r;

  *window_id = buffer[4];
  const u32 length_be = ((u32) buffer[9]) << 16 |
                        ((u32) buffer[10]) << 8 |
                        ((u32) buffer[11]);
  *length = ntohl(length_be);

  return 0;
}

int
test_unit_ready(int fd, u8 *requestsense) {
  static const u8 command[] = {0, 0, 0, 0, 0, 0};
  return scsi_command(fd, SG_DXFER_FROM_DEV, command, sizeof(command), NULL, 0,
                      requestsense, 0);
}

int
kvss905c_read(int fd, u8 type, u8 q1, u8 q2, u8 *buffer, u32 length, u8 *requestsense) {
  // see page 50
  const u8 command[] = {0x28, 0, type, 0, q1, q2, length >> 16, length >> 8, length, 0};

  return scsi_command(fd, SG_DXFER_FROM_DEV, command, sizeof(command),
                             buffer, length, requestsense, 0);
}

#define KVSS905C_READ_IMAGE 0
#define KVSS905C_READ_PICTURE_ELEMENT_SIZE 0x80
#define KVSS905C_READ_SUPPORT 0x93

int
kvss905c_picture_size(int fd, u8 page, u8 back, u32 *width, u32 *height,
                      u8 *requestsense) {
  // see page 50
  u8 buffer[16];
  memset(buffer, 0, sizeof(buffer));

  const int r = kvss905c_read(fd, KVSS905C_READ_PICTURE_ELEMENT_SIZE, page,
                              back ? 0x80 : 0, buffer, sizeof(buffer), requestsense);
  if (r) return r;
  const u32 width_be = *((u32 *) buffer);
  const u32 height_be = *((u32 *) &buffer[4]);

  if (width) *width = ntohl(width_be);
  if (height) *height = ntohl(height_be);

  return 0;
}

#define UNSAFE_MIN(x, y) x < y ? x : y

// -----------------------------------------------------------------------------
// Poll the scanner until it has data to send us
// -----------------------------------------------------------------------------
int
kvss905c_data_buffer_wait(int fd, u8 *requestsense) {
  u32 length;
  u8 window_id;

  for (;;) {
    if (get_data_buffer_status(fd, &window_id, &length, requestsense)) {
      return 1;
    }
    if (length) break;
    usleep(5000);
  }

  return 0;
}

int kvss905c_read_data(int fd, u8 page, u8 back, u8 *buffer, unsigned blen,
                       unsigned *result, char *eof, u8 *requestsense) {
  const unsigned length = UNSAFE_MIN(kMaxBuffer, blen);
  if (kvss905c_read(fd, KVSS905C_READ_IMAGE, page, back ? 0x80 : 0, buffer,
                    length, requestsense)) {
    if (requestsense[0] == 0xf0 && requestsense[2] == 0x60) {
      // End of Medium and ILI
      const u32 delta = ntohl( *((u32 *) &requestsense[3]));
      *result = length - delta;
      *eof = 1;
      return 0;
    }
    return 1;
  }
  *result = length;
  *eof = 0;

  return 0;
}

int
kvss905c_detect(int fd) {
  // see page 28
  u8 requestsense[REQUEST_SENSE_SIZE];

  u8 inquirydata[96];
  memset(inquirydata, 0, sizeof(inquirydata));
  static const u8 command[] = {0x12, 0, 0, 0, 0x60, 0};

  const int r = scsi_command(fd, SG_DXFER_FROM_DEV, command, sizeof(command),
                             inquirydata, sizeof(inquirydata), requestsense, 0);
  if (r) return r;

  // We check that the model string begins with "KV-"
  return memcmp(inquirydata + 16, "KV-", 3);
}

int
kvss905c_stop(int fd, u8 *requestsense) {
  // see page 89
  static const u8 command[] = {0xe1, 0, 0x8b, 0, 0, 0, 0, 0, 0, 0};
  return scsi_command(fd, SG_DXFER_TO_DEV, command, sizeof(command),
                      NULL, 0, requestsense, 0);
}

int
kvss905c_open() {
  // We find the scanner by opening all the SCSI generic devices and seeing if
  // kvss905c_detect thinks that it's a good bet

  char devicename[64];
  devicename[63] = 0;

  for (unsigned i = 0; ; ++i) {
    snprintf(devicename, sizeof(devicename) - 1, "/dev/sg%d", i);
    const int fd = open(devicename, O_RDWR);
    if (fd < 0) {
      // If we have run out of SCSI generic devices, give up
      if (errno == ENOENT) return -1;
      // Many other errors (e.g. EPERM) mean that we try the next one
      continue;
    }

    if (kvss905c_detect(fd) == 0) return fd;
    close(fd);
  }
}

const char *
kvss905c_strerror(u8 *requestsense) {
  // Table taken from page 25
  const u16 error = scsi_error_code(requestsense);
  const u8 sense = requestsense[2] & 0x0f;

  switch (error) {
    case 0x2400: return "Invalid field in CDB";
    case 0x3a00: return "Out of paper";
    case 0x0480: return "Document lead door open";
    case 0x0481: return "Document discharge door open";
    case 0x0482: return "Post imprinter door open";
    case 0x8001:
      if (sense == 2) return "Scanner stopped";
      else if (sense == 3) return "Jammed at document lead";
      else if (sense == 4) return "Lamp failure with regular temperature";
      else return NULL;
    case 0x8002:
      if (sense == 2) return "Document feeder stopped";
      else if (sense == 3) return "Jammed at document discharge 1";
      else if (sense == 4) return "Document size detect error";
      else return NULL;
    case 0x8003: return "Jammed at document discharge 2";
    case 0x8004:
      if (sense == 3) return "Document internal rest";
      else if (sense == 4) return "Document hopper error";
      else return NULL;
    case 0x8005:
      if (sense == 3) return "Jammed at document feed 1";
      if (sense == 4) return "Document sensor adjust error";
      else return NULL;
    case 0x8006: return "Jammed at document feed 2";
    case 0x8007: return "Jammed at document feed 3";
    case 0x8008: return "Jammed at document feed 4";
    case 0x800a: return "Skew error";
    case 0x800b: return "Minimum media error";
    case 0x800c: return "Media length error";
    case 0x800d: return "Double feed error";
    case 0x800e: return "Barcode error";
    case 0x0880: return "Internal parameter error";
    case 0x0881: return "Internal DMA error";
    case 0x0882: return "Internal command error";
    case 0x8083: return "Internal communication error";
    case 0x4480: return "Internal RAM error";
    case 0x4481: return "Internal EEPROM error";
    case 0x4482: return "FPGA error";
    case 0x4700: return "SCSI parity error";
    case 0x1a00: return "Parameter list length error";
    case 0x2000: return "Invalid command op code";
    case 0x2500: return "Logical unit not supported";
    case 0x2600: return "Invalid field in parameter list";
    case 0x2c01: return "Too many windows";
    case 0x2c02: return "Invalid window combination";
    case 0x2c80: return "Out of memory";
    case 0x2c81: return "No back scanning unit";
    case 0x2c82: return "No imprinter unit";
    case 0x2c83: return "Pointer position error";
    case 0x2c84: return "Out of scanning page limit";
    case 0x2c85: return "Out of scanning length limit";
    case 0x2c86: return "Out of scanning resolution limit";
    case 0x2c87: return "Out of scanning line cycle limit";
    case 0x3d00: return "Invalid bits in identity message";
    case 0x2900: return "Unit attention";
    case 0x1b00: return "Sync data transfer error";
    case 0x4300: return "Message error";
    case 0x4900: return "Invalid message error";
    default: return NULL;
  }
}
