// Author: agl@imperialviolet.org (Adam Langley)
// Author: dgluss@google.com (David Gluss)
//
// Copyright (C) 2007, 2011 Google Inc.
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
#include <libusb-1.0/libusb.h>
#include <arpa/inet.h>

#include <scsi/sg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#ifndef SG_FLAG_MMAP_IO
#define SG_FLAG_MMAP_IO 4
#endif

#include "kvs3105usb.h"

static const unsigned int kMaxBuffer = 0x10000;

// -----------------------------------------------------------------------------
// This is a series of utility functions for dealing with the Panasonic kvs3105
// USB sheetfeed scanner.
// The scanner appears to tunnel SCSI commands over the USB channel.
// This code uses libusb-1.0
// Make sure that udev recognizes the scanner, with
// something like this:
// SYSFS{idVendor}=="04da", SYSFS{idProduct}=="1004", MODE="0660",
//       GROUP="plugdev"
// in a .rules file in /etc/udev/rules.d, or something along those lines,
// to deal with permissions problems.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Dump the given requestsense buffer to stderr in a, hopefully, useful format.
// The requestsense buffer should result from a failed SCSI operation
// This function is just used for debugging.
// -----------------------------------------------------------------------------
void scsi_usb_request_sense_dump(void *irequestsense) {
  const uint8_t *requestsense = (uint8_t *) irequestsense;

  fprintf(stderr, "Request failed: ");
  for (int i = 0; i < KVS3105_REQUEST_SENSE_SIZE; ++i) {
    fprintf(stderr, "%02x ", requestsense[i]);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "ASC/ASCQ: %02x %02x\n",
          requestsense[12], requestsense[13]);
  fprintf(stderr, "See http://www.t10.org/lists/asc-num.htm\n");
}

// -----------------------------------------------------------------------------
// Returns the SCSI error code from a request-sense-buffer.
// See: http://www.t10.org/lists/asc-num.htm
// -----------------------------------------------------------------------------
uint16_t scsi_usb_error_code(const uint8_t *requestsense) {
  return (requestsense[12] << 8) | requestsense[13];
}

const unsigned int COMMAND_BLOCK = 1;
const unsigned int DATA_BLOCK = 2;
const unsigned int RESPONSE_BLOCK = 3;

const unsigned int COMMAND_CODE = 0x9000;
const unsigned int DATA_CODE = 0xb000;
const unsigned int RESPONSE_CODE = 0xa000;
const unsigned int STATUS_SIZE = 4;

struct bulk_header {
  uint32_t length;
  uint16_t type;
  uint16_t code;
  uint32_t transaction_id;
}__attribute__((packed));

const unsigned int GOOD = 0;
const unsigned int CHECK_CONDITION = 2;

typedef enum {
  CMD_NONE = 0,
  CMD_IN = 0x81,                /* scanner to pc */
  CMD_OUT = 0x02                /* pc to scanner */
} CMD_DIRECTION; /* equals to endpoint address */

#define RESPONSE_SIZE 0x12
const unsigned int MAX_CMD_SIZE = 12;

struct cmd {
  const unsigned char *cmd;
  int cmd_size;
  void *data;
  int data_size;
  int dir;
};

struct response {
  int status;
  unsigned char data[RESPONSE_SIZE];
};

const unsigned int REQUEST_SENSE = 0x03;
// Send a SCSI command encapsulated in a USB packet.
// Return codes:
// -3 failure to send the command
// -2 failure to send or receive the associated data
// -1 failure to get the SCSI status back through USB
// 0  everything OK
static int usb_send_command(usb_handle usbhandle, struct cmd *c,
                            struct response *r, void *buf, int timeout) {
  struct bulk_header *h = (struct bulk_header *) buf;
  uint8_t resp[sizeof(*h) + STATUS_SIZE];
  size_t sz = sizeof(*h) + MAX_CMD_SIZE;
  memset(h, 0, sz);
  h->length = htonl(sz);
  h->type = htons(COMMAND_BLOCK);
  h->code = htons(COMMAND_CODE);
  memcpy(h + 1, c->cmd, c->cmd_size);
  if(!timeout)
    timeout = 10000;  // ten second timeout by default
  int transferred;
  // Transfer the command itself to the USB device.
  int ret1 = libusb_bulk_transfer(usbhandle, CMD_OUT, (unsigned char *)h,
                                  sz, &transferred, timeout);
  if (ret1) {
    fprintf(stderr, "  failed to send command, "
            "libusb_bulk_transfer returned %d\n", ret1);
    return -3;
  }

  // If the direction of data transfer is IN, then get data from the device.
  if (c->dir == CMD_IN) {
    sz = sizeof(*h) + c->data_size;

    ret1 = libusb_bulk_transfer(usbhandle, CMD_IN, (unsigned char *)h, sz,
                                &transferred, timeout);
    c->data = h + 1;

    if (ret1 || transferred < sizeof(*h)) {
      if (ret1)
        fprintf(stderr, "  failed to transfer data IN, libusb error: %d %s\n"
                "  transferred: %d\n",
                ret1, kvs3105_libusb_error_string(ret1), transferred);
      r->status = CHECK_CONDITION;
      c->data_size = 0;
      return -2;
    }

    c->data_size = sz - sizeof(*h);

  } else if (c->dir == CMD_OUT) {
    // If we're to send data out, then send it.
    sz = sizeof(*h) + c->data_size;

    memset(h, 0, sizeof(*h));
    h->length = htonl(sizeof(*h) + c->data_size);
    h->type = htons(DATA_BLOCK);
    h->code = htons(DATA_CODE);
    memcpy(h + 1, c->data, c->data_size);
    ret1 = libusb_bulk_transfer(usbhandle, CMD_OUT, (unsigned char *)h,
                                sz, &transferred, timeout);
    if (ret1) {
      fprintf(stderr, "  failed to transfer data OUT, libusb error: %d %s\n",
              ret1, kvs3105_libusb_error_string(ret1));
      return -2;
    }
  }

  sz = sizeof(resp);
  // Get the SCSI status packet.
  ret1 = libusb_bulk_transfer(usbhandle, CMD_IN, (unsigned char *)resp,
                              sz, &transferred, timeout);
  if (ret1) {
    fprintf(stderr, "Error getting SCSI status packet. code %d: %s\n", ret1,
            kvs3105_libusb_error_string(ret1));
    r->status = CHECK_CONDITION;
    return -1;
  }
  r->status = ntohl(*((uint32_t *) (resp + sizeof(*h))));
  return 0;
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
//   3 if the data transfer failed
// -----------------------------------------------------------------------------
static int send_command(usb_handle usbhandle, int direction,
                        const void *command, unsigned command_length,
                        void *data, unsigned data_length,
                        void *requestsense, int timeout) {
  int st;
  uint8_t *bb = alloca(sizeof(struct bulk_header) +
                       (data_length > MAX_CMD_SIZE?
                        data_length:MAX_CMD_SIZE));
  struct response r = {};
  struct cmd c = {
    .cmd = command,
    .cmd_size = command_length,
    .dir = !data || !data_length ? CMD_NONE:
    direction == SG_DXFER_TO_DEV
    ? CMD_OUT : CMD_IN,
    .data = data,
    .data_size = data_length
  };
  memset(requestsense, 0, RESPONSE_SIZE);

  st = usb_send_command(usbhandle, &c, &r, bb, timeout);

  // usb_send_command returns -2 on failure to send or receive the
  // associated data.  This is a data transfer failure (distinguished
  // from a protocol error, USB or SCSI). This can happen during a
  // paper jam, when the book is finished, and perhaps other
  // situations.
  if (st == -2)
    return 3;
  if (st < -1) {
    fprintf(stderr, "usb_send_command returned %d\n", st);
    return 1;
  }
  if (c.dir == CMD_IN)
    memcpy(data, c.data, c.data_size);

  if (r.status) {
    // Okay, something didn't go 100% awesomely, so we
    // ask the scanner, hey, what is your status? Are
    // you suffering? Do you miss your mommy? Whatever
    // answer we get (in SCSI speak) is stuffed in
    // requestsense datastructure. Happens whenever
    // the data cable is too slow and we are forced
    // to wait.
    uint8_t b[sizeof(struct bulk_header) + RESPONSE_SIZE];
    uint8_t cmd[6]={REQUEST_SENSE, 0,0,0, RESPONSE_SIZE};
    struct cmd c2 = {
      .cmd = cmd,
      .cmd_size = 6,
      .dir = CMD_IN,
      .data_size = RESPONSE_SIZE,
    };

    st = usb_send_command(usbhandle, &c2, &r, b, timeout);
    if (st < -1 || !c2.data_size) {
      if (st < -1)
        fprintf(stderr, "usb_send_command returned %d\n", st);
      else
        fprintf(stderr, "data_size was 0\n");
      return 1;
    }
    memcpy(requestsense, b + sizeof(struct bulk_header),
           RESPONSE_SIZE);
    if (r.status)
      fprintf(stderr, "r.status is now %d\n", r.status);

    return 2;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// KVS3105 specific function. See the header file for comments...

void kvs3105_window_init(struct kvs3105_window *window) {
  memset(window, 0, sizeof(struct kvs3105_window));

  window->composition = KVS3105_COMPOSITION_COLOUR;
  window->bpp = 24;
  window->xres = window->yres = 300;  // 300 dpi
  window->number_of_pages_to_scan = 0xff;  // all pages
  window->emphasis = 0xf0;  // No conversion
  window->document_size = 7;  // US letter
  window->double_feed_detector = 1;
  window->subsample = 3;  // 4:2:2 JPEG subsampling
  window->flatbed = 0;  // non-flatbed

  // US letter sizes
  window->document_width = window->width = 8.5*1200;
  window->document_length = window->length = 11*1200;

  window->compression_type = 0x81;  // JPEG
  window->compression_argument = 85;  // quality 85
}

static int kvs3105_window_serialise(uint8_t *output,
                                    const struct kvs3105_window *window) {
  unsigned j = 0;
#define U8(x) output[j++] = window->x;
#define U16(x) do { uint16_t t = htons(window->x); memcpy(output + j, &t, 2); \
    j += 2; } while (0);
#define U32(x) do { uint32_t t = htonl(window->x); memcpy(output + j, &t, 4); \
    j += 4; } while (0);
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
      window->lamp << 4 | window->double_feed_sensitivity << 0);
  U8I(window->remove_moire << 6 |
      window->subsample << 4 | window->colour_match << 0);
  U8(document_size);
  U32(document_width);
  U32(document_length);
  U8I(window->ahead_disable << 7 |
      window->deskew << 5 |
      window->double_feed_detector << 4 |
      window->full_size_scan << 2 |
      window->feed_slow << 1 | window->remove_shadow << 0);
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

int kvs3105_reset_windows(usb_handle usbhandle,
                          uint8_t *requestsense) {
  const uint8_t command[] = { 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  const int r = send_command(usbhandle, SG_DXFER_TO_DEV, command,
                             sizeof(command), 0, 0, requestsense, 0);
  return r;
}

int kvs3105_set_windows(usb_handle usbhandle,
                        const struct kvs3105_window *window,
                        char duplex,
                        uint8_t *requestsense) {
  // see page 35
  uint8_t windowbytes[6 + 2 + WINDOW_SIZE];
  memset(windowbytes, 0, sizeof(windowbytes));

  const int bytes_written = kvs3105_window_serialise(windowbytes + 8, window);
  if (bytes_written != WINDOW_SIZE)
    abort();

  const uint16_t length = htons(WINDOW_SIZE);
  memcpy(windowbytes + 6, &length, sizeof(length));
  static const int transfer_length = sizeof(windowbytes);

  const uint8_t command[] = { 0x24, 0, 0, 0, 0, 0, transfer_length >> 16,
                              transfer_length >> 8, transfer_length, 0 };
  const int r = send_command(usbhandle, SG_DXFER_TO_DEV, command,
                             sizeof(command), windowbytes, transfer_length,
                             requestsense, 0);

  if (r)
    return r;

  if (duplex) {
    windowbytes[8] = 0x80;
    return send_command(usbhandle, SG_DXFER_TO_DEV, command, sizeof(command),
                        windowbytes, transfer_length, requestsense, 0);
  }

  return 0;
}

int kvs3105_scan(usb_handle usbhandle, uint8_t *requestsense) {
  // see page 33
  static const uint8_t command[] = {0x1b, 0, 0, 0, 0, 0};

  return send_command(usbhandle, SG_DXFER_TO_DEV, command, sizeof(command),
                      NULL, 0, requestsense, 0);
}

static int get_data_buffer_status(usb_handle usbhandle, uint8_t *window_id,
                                  uint32_t *length, uint8_t *requestsense) {
  // see page 71
  uint8_t buffer[12];
  static const uint8_t command[] = { 0x34, 0, 0, 0, 0, 0,
                                     0, 0, sizeof(buffer), 0 };
  const int r = send_command(usbhandle, SG_DXFER_FROM_DEV, command,
                             sizeof(command), buffer, sizeof(buffer),
                             requestsense, 0);
  if (r)
    return r;

  *window_id = buffer[4];
  const uint32_t length_be = ((uint32_t) buffer[9]) << 16 |
      ((uint32_t) buffer[10]) << 8 |
      ((uint32_t) buffer[11]);
  *length = length_be;

  return 0;
}

int kvs3105_unit_not_ready(usb_handle usbhandle) {
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  static const uint8_t command[] = { 0, 0, 0, 0, 0, 0 };
  int retval = send_command(usbhandle, SG_DXFER_FROM_DEV, command,
                            sizeof(command), NULL, 0, requestsense, 0);
  return retval;
}

int kvs3105_read(usb_handle usbhandle, uint8_t type, uint8_t q1, uint8_t q2,
                 uint8_t *buffer, uint32_t length, uint8_t *requestsense) {
  // see page 50
  const uint8_t command[] = { 0x28, 0, type, 0, q1, q2,
                              length >> 16, length >> 8, length, 0 };

  return send_command(usbhandle, SG_DXFER_FROM_DEV, command,
                                  sizeof(command), buffer, length,
                                  requestsense, 0);
}

const unsigned int KVS3105_READ_IMAGE = 0;
const unsigned int KVS3105_READ_PICTURE_ELEMENT_SIZE = 0x80;
const unsigned int KVS3105_READ_SUPPORT = 0x93;

int kvs3105_picture_size(usb_handle usbhandle, uint8_t page, uint8_t back,
                         uint32_t *width, uint32_t *height,
                         uint8_t *requestsense) {
  // see page 50
  union {
    uint8_t chars[16];
    struct {
      uint32_t width;
      uint32_t height;
    } s;
  } buffer;
  memset(buffer.chars, 0, sizeof(buffer));

  const int r = kvs3105_read(usbhandle, KVS3105_READ_PICTURE_ELEMENT_SIZE, page,
                             back ? 0x80 : 0, buffer.chars, sizeof(buffer),
                             requestsense);
  if (r)
    return r;

  if (width)
    *width = ntohl(buffer.s.width);
  if (height)
    *height = ntohl(buffer.s.height);

  return 0;
}

#define UNSAFE_MIN(x, y) x < y ? x : y

// -----------------------------------------------------------------------------
// Poll the scanner until it has data to send us
// -----------------------------------------------------------------------------
int kvs3105_data_buffer_wait(usb_handle usbhandle, uint8_t *requestsense) {
  uint32_t length;
  uint8_t window_id;

  for (;;) {
    int return_code = get_data_buffer_status(usbhandle, &window_id,
                                             &length, requestsense);
    if (return_code)
      return return_code;
    if (length)
      break;
    usleep(50000);  // usleep is in microseconds
  }
  return 0;
}

int kvs3105_read_data(usb_handle usbhandle, uint8_t page, uint8_t back,
                      uint8_t *buffer, unsigned blen, unsigned *result,
                      char *end_of_page, uint8_t *requestsense) {
  const unsigned length = UNSAFE_MIN(kMaxBuffer, blen);
  if (kvs3105_read(usbhandle, KVS3105_READ_IMAGE, page, back ? 0x80 : 0, buffer,
                   length, requestsense)) {
    char current_error = (requestsense[0] == 0xf0) ? 1:0;
    char end_of_medium = (requestsense[2] >> 6) & 1;
    char incorrect_length_indicator = (requestsense[2] >> 5) & 1;
    if (current_error && incorrect_length_indicator) {
      const uint32_t delta = ntohl( *((uint32_t *) &requestsense[3]));
      *result = length - delta;
      *end_of_page = end_of_medium;
      return 0;
    }
    fprintf(stderr, "Unexpected read error\n");
    scsi_usb_request_sense_dump(requestsense);
    return 1;
  }
  *result = length;
  *end_of_page = 0;

  return 0;
}

int kvs3105_detect(usb_handle usbhandle) {
  // see page 28
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];

  uint8_t inquirydata[96];
  memset(inquirydata, 0, sizeof(inquirydata));
  static const uint8_t command[] = {0x12, 0, 0, 0, 0x60, 0};

  const int r = send_command(usbhandle, SG_DXFER_FROM_DEV, command,
                             sizeof(command), inquirydata, sizeof(inquirydata),
                             requestsense, 0);
  if (r) return r;

  // We check that the model string begins with "KV-"
  return memcmp(inquirydata + 16, "KV-", 3);
}

int kvs3105_stop(usb_handle usbhandle, uint8_t *requestsense) {
  // see page 89
  static const uint8_t command[] = {0xe1, 0, 0x8b, 0, 0, 0, 0, 0, 0, 0};
  return send_command(usbhandle, SG_DXFER_TO_DEV, command, sizeof(command),
                      NULL, 0, requestsense, 0);
}

libusb_device *find_3105_scanner(libusb_device **device_list, size_t cnt,
                                 const char *name) {
  libusb_device *found = NULL;
  int seekbus = -1, seekdev = -1;
  if (name && *name)
    sscanf(name, "%d:%d", &seekbus, &seekdev);
  for (int i = 0; i < cnt; i++) {
    libusb_device *device = device_list[i];
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor (device, &desc);
    if (desc.idVendor == KVS3105_VENDOR_ID) {
      if (desc.idProduct == KVS3105_ID ||
          desc.idProduct == KVS70XX_ID ) {
        if (seekbus >= 0 &&
            seekbus != libusb_get_bus_number(device))
          continue;
        if (seekdev >= 0 &&
            seekdev != libusb_get_device_address(device))
          continue;
        found = device;
        break;
      }
    }
  }
  return found;
}

usb_handle kvs3105_open(const char *name) {
  // Use libusb
  libusb_init(0);
  // discover devices
  libusb_device **device_list;
  libusb_device *found = NULL;
  ssize_t cnt;
  cnt = libusb_get_device_list(NULL, &device_list);
  int err = 0;
  if (cnt < 0)
    return NULL;

  usb_handle handle;
  found = find_3105_scanner(device_list, cnt, name);
  if (found) {
    err = libusb_open(found, &handle);
    if (err) {
      perror("Can't open scanner device");
      found = NULL;
    }
  }
  libusb_free_device_list(device_list, 1);
  if (!found)
    return NULL;

  /* Claim USB interface 0 */
  if (libusb_claim_interface(handle, 0)) {
    perror("Can not claim interface");
    libusb_close(handle);
    return NULL;
  }
  for (int i = 0 ; i < 10; i++) {
    if (!kvs3105_unit_not_ready(handle))
      return handle;
    sleep(2);
  }
  libusb_release_interface(handle, 0);
  libusb_close(handle);
  return NULL;
}

void kvs3105_reset(const char *name) {
  usb_handle handle;
  libusb_device **device_list;
  libusb_device *found = NULL;
  libusb_init(0);
  ssize_t cnt = libusb_get_device_list(NULL, &device_list);
  int err = 0;
  if (cnt < 0)
    return;
  found = find_3105_scanner(device_list, cnt, name);
  if (found) {
    err = libusb_open(found, &handle);
    if (err) {
      found = NULL;
    }
  }
  libusb_free_device_list(device_list, 1);
  if (!found)
    return;
  if (!handle)
    return;
  libusb_reset_device(handle);
  usleep(500000);
}

void kvs3105_clear_halt(usb_handle h) {
  libusb_clear_halt(h, CMD_IN);
  libusb_clear_halt(h, CMD_OUT);
}

void kvs3105_close(usb_handle h) {
  libusb_release_interface(h, 0);
  libusb_close(h);
}

const char *kvs3105_strerror(uint8_t *requestsense) {
  // Table taken from page 25
  const uint16_t error = scsi_usb_error_code(requestsense);
  const uint8_t sense = requestsense[2] & 0x0f;

  if (sense == 0) {
    switch (error) {
      case 0x0000: return "Sense code 0 returned";
    }
  }

  if (sense == 2) {
    switch (error) {
      case 0x0000: return "Not ready";
      case 0x0401: return "Logical unit is in process of becoming ready";
      case 0x0480: return "Document lead door open";
      case 0x0481: return "Document discharge door open";
      case 0x0482: return "Post imprinter door open";
      case 0x8001: return "Scanner stopped";
      case 0x8002: return "ADF stopped";
    }
  }

  if (sense == 3) {
    switch (error) {
      case 0x3a00: return "Out of paper";
      case 0x8001: return "Jammed at document lead";
      case 0x8002: return "Jammed at document discharge 1";
      case 0x8003: return "Jammed at document discharge 2";
      case 0x8004: return "Document internal rest";
      case 0x8006: return "Jammed at document feed 1";
      case 0x8007: return "Jammed at document feed 2";
      case 0x8008: return "Jammed at document feed 3";
      case 0x8009: return "Jammed at document feed 4";
      case 0x800a: return "Skew error";
      case 0x800b: return "Minimum media error";
      case 0x800c: return "Media length error";
      case 0x800d: return "Double feed error";
      case 0x800e: return "Barcode error";
    }
  }

  if (sense == 4) {
    switch (error) {
      case 0x0880: return "Internal parameter error";
      case 0x0881: return "Internal DMA error";
      case 0x0882: return "Internal command error";
      case 0x8083: return "Internal communication error";
      case 0x4480: return "Internal RAM error";
      case 0x4481: return "Internal EEPROM error";
      case 0x4482: return "FPGA error";
      case 0x4700: return "SCSI parity error";
      case 0x8001: return "Lamp failure with regular temperature";
      case 0x8002: return "Document size detect error";
      case 0x8004: return "Document hopper error";
      case 0x8005: return "Document sensor adjust error";
    }
  }

  if (sense == 5) {
    switch (error) {
      case 0x1a00: return "Parameter list length error";
      case 0x2000: return "Invalid command op code";
      case 0x2400: return "Invalid field in CDB";
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
    }
  }

  if (sense == 6) {
    switch (error) {
      case 0x2900: return "Unit attention";
      case 0x1b00: return "Sync data transfer error";
      case 0x4300: return "Message error";
      case 0x4900: return "Invalid message error";
      case 0x8001: return "Image data transfer error";
    }
  }

  return NULL;
}

// Returns a malloc'd buffer that should be freed.
char* list_3105_devices() {
  char *return_buffer=strdup("");
  char tmp_buf[64];  // ought to do it for two ints
  libusb_device **device_list;
  libusb_init(0);
  int cnt = libusb_get_device_list(NULL, &device_list);
  int i;
  for (i = 0; i < cnt; i++) {
    libusb_device *device = device_list[i];
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor (device, &desc);
    if (desc.idVendor == KVS3105_VENDOR_ID) {
      if (desc.idProduct == KVS3105_ID ||
          desc.idProduct == KVS70XX_ID ) {
        int len;
        sprintf(tmp_buf, "%d:%d\n",
                libusb_get_bus_number(device),
                libusb_get_device_address(device));
        len = strlen(return_buffer);
        return_buffer = realloc(return_buffer, len + strlen(tmp_buf) + 1);
        strcpy(return_buffer + len, tmp_buf);
      }
    }
  }
  if (cnt <= 0)
    return strdup("No devices found\n");
  return return_buffer;
}

// This ought to be part of libusb, of course.
const char *kvs3105_libusb_error_string(int error_number) {
  switch (error_number) {
        /** Success (no error) */
    case LIBUSB_SUCCESS:
      return "LIBUSB_SUCCESS";

        /** Input/output error */
    case LIBUSB_ERROR_IO:
      return "LIBUSB_ERROR_IO";

        /** Invalid parameter */
    case LIBUSB_ERROR_INVALID_PARAM:
      return "LIBUSB_ERROR_INVALID_PARAM";

        /** Access denied (insufficient permissions) */
    case LIBUSB_ERROR_ACCESS:
      return "LIBUSB_ERROR_ACCESS";

        /** No such device (it may have been disconnected) */
    case LIBUSB_ERROR_NO_DEVICE:
      return "LIBUSB_ERROR_NO_DEVICE";

        /** Entity not found */
    case LIBUSB_ERROR_NOT_FOUND:
      return "LIBUSB_ERROR_NOT_FOUND";

        /** Resource busy */
    case LIBUSB_ERROR_BUSY:
      return "LIBUSB_ERROR_BUSY";

        /** Operation timed out */
    case LIBUSB_ERROR_TIMEOUT:
      return "LIBUSB_ERROR_TIMEOUT";

        /** Overflow */
    case LIBUSB_ERROR_OVERFLOW:
      return "LIBUSB_ERROR_OVERFLOW";

        /** Pipe error */
    case LIBUSB_ERROR_PIPE:
      return "LIBUSB_ERROR_PIPE";

        /** System call interrupted (perhaps due to signal) */
    case LIBUSB_ERROR_INTERRUPTED:
      return "LIBUSB_ERROR_INTERRUPTED";

        /** Insufficient memory */
    case LIBUSB_ERROR_NO_MEM:
      return "LIBUSB_ERROR_NO_MEM";

        /** Operation not supported or unimplemented on this platform */
    case LIBUSB_ERROR_NOT_SUPPORTED:
      return "LIBUSB_ERROR_NOT_SUPPORTED";

        /** Other error */
    case LIBUSB_ERROR_OTHER:
      return "LIBUSB_ERROR_OTHER";
  }

  return "LIBUSB_UNKNOWN_ERROR";
};

const char *kvs3105_scsi_command_string(unsigned int cmd, unsigned int subcmd) {
  switch (cmd) {
    case 0x00:
      return "TEST UNIT READY";
    case 0x03:
      return "REQUEST SENSE";
    case 0x12:
      return "INQUIRY";
    case 0x16:
      return "RESERVE UNIT";
    case 0x17:
      return "RELEASE UNIT";
    case 0x1B:
      return "SCAN";
    case 0x1D:
      return "SEND DIAGNOSTIC";
    case 0x24:
      return "SET WINDOW";
    case 0x28:
      return "READ";
    case 0x2A:
      return "SEND";
    case 0x31:
      return "OBJECT POSITION";
    case 0x34:
      return "GET DATA BUFFER STATUS";
    case 0xC0:
      return "SET SUBWINDOW";
    case 0xE0:
      switch (subcmd) {
        case 0x83:
          return "GET VERSION";
        case 0x86:
          return "GET COUNTER";
        case 0x90:
          return "GET WARNING";
        case 0xA0:
          return "GET BACKGROUND LEVEL";
        default:
          return "UNKNOWN 0xE0 COMMAND";
      }
    case 0xE1:
      switch (subcmd) {
        case 0x05:
        case 0x07:
          return "HOPPER DOWN";
        case 0x85:
          return "SET TIME";
        case 0x8B:
          return "STOP ADF";
        case 0x91:
          return "CLEAR WARNING";
        case 0x8D:
          return "SET TIMEOUT";
        default:
          return "UNKNOWN 0xE1 COMMAND";
      }
    case 0xE4:
      return "SET IMPRINTER";
    case 0xE6:
      return "SET BARCODE";
    default:
      return "UNKNOWN COMMAND";
  }
}
