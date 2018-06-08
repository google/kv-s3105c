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

// This code is an example of how to use the kvs3105 library.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include <stdint.h>

#include "kvs3105usb.h"

int usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [options] filebase\n"
          "  -d <device number to use>\n"
          "  -n <number of pages to scan>\n"
          "  -p <first page number> (zero based)\n"
          "  -q <quality>: percent 1-100\n"
          "  -b <block size>\n"
          "  -w <width in inches>\n"
          "  -h <height in inches>\n"
          "  -c <compression type> (0x81 is jpeg)\n"
          "  -s (output to stdout)\n"
          "  -r <resolution> (e.g. 300)\n"
          "  -f scan from flatbed\n"
          "  -i or --interactive: interactive mode\n"
          "  --list: show USB devices\n"
          "  --duplex: scan front and back\n",
          argv0);
  return 1;
}


void report(const char *comment, uint8_t *requestsense) {
  const char *msg = kvs3105_strerror(requestsense);
  int code = (int) scsi_usb_error_code(requestsense);
  fprintf(stderr, "%s: %x\n", comment, code);
  if (msg) {
    fprintf(stderr, "%s\n", msg);
  }
}

usb_handle reset_and_attach(const char *devicename) {
  kvs3105_reset(devicename);
  return kvs3105_open(devicename);
}

int main(int argc, char **argv) {
  float width = 8.5, height = 11.0;
  unsigned num_pages = 1;
  unsigned block_size = 1;
  int interactive_mode = 0;
  int duplex = 0;
  int list = 0;
  int quality = 90;
  int pixels_per_inch = 400;
  int flatbed = 0;
  int compression_type = 0x81;  // JPEG
  int output_to_stdout = 0;

  int first_page_number = 0;
  const char *device_name = 0;
  struct option longopts[] = {
    { "duplex", 0, &duplex, 1 },
    { "list", 0, &list, 1 },
    { "interactive", 0, &interactive_mode, 1 },
    { 0 } };

  int opt;
  while ((opt = getopt_long(argc, argv,
                            "d:n:p:q:b:w:h:c:sr:if",
                            longopts,
                            NULL)) != -1) {
    switch (opt) {
      case 0:  // it was a long option, already handled!
        break;
      case 'd':
        device_name = optarg;
        break;
      case 'n':
        num_pages = atoi(optarg);
        break;
      case 'p':
        first_page_number = atoi(optarg);
        break;
      case 'q':
        quality = atoi(optarg);
        break;
      case 'b':
        block_size = atoi(optarg);
        break;
      case 'w':
        width = strtof(optarg, NULL);
        break;
      case 'h':
        height = strtof(optarg, NULL);
        break;
      case 'c':
        compression_type = atoi(optarg);
        break;
      case 's':
        output_to_stdout = 1;
        break;
      case 'r':
        pixels_per_inch = atoi(optarg);
        break;
      case 'f':
        flatbed = 1;
        break;
      case 'i':
        interactive_mode++;
        break;
      default:
        fprintf(stderr, "Unknown option: %s\n", optarg);
        return usage(argv[0]);
    }
  }

  if (list) {
    char *mylist = list_3105_devices();
    fprintf(stdout, "%s", mylist);
    free(mylist);
    exit(0);
  }

  if (interactive_mode) {
    do_interactive();
    exit(0);
  }

  if (optind >= argc && !output_to_stdout)
    return usage(argv[0]);

  const char *const filebase = argv[optind];

  usb_handle uh = reset_and_attach(device_name);

  if (uh == NULL) {
    fprintf(stderr, "Cannot open scanner\n");
    return 2;
  }

  struct kvs3105_window window;
  kvs3105_window_init(&window);

  window.document_length = window.length = height * 1200;
  window.document_width = window.width = width * 1200;
  window.compression_argument = quality;
  window.compression_type = compression_type;

  // match the behavior of sheetfed_server
  window.emphasis = 0xf0;
  window.subsample = 0;
  window.xres = window.yres = pixels_per_inch;
  window.flatbed = flatbed;

  if (block_size > 254) {
    block_size = num_pages;
    if (num_pages > 254) {
      window.number_of_pages_to_scan = 0xff;
    } else {
      window.number_of_pages_to_scan = num_pages;
    }
  } else {
    window.number_of_pages_to_scan = block_size;
  }

  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  for (unsigned pageno = first_page_number;
       pageno < first_page_number + num_pages;) {
    if (kvs3105_reset_windows(uh, requestsense)) {
      report("Error resetting windows", requestsense);
      return 2;
    }
    if (kvs3105_set_windows(uh, &window, duplex, requestsense)) {
      report("Error setting windows", requestsense);
      return 2;
    }
    if (kvs3105_scan(uh, requestsense)) {
      report("Error starting scanning", requestsense);
      return 2;
    }

    // We scan in blocks of block_size pages
    int side = 0;
    for (unsigned page = 0; page < block_size;) {
      uint32_t width, height;
      if (kvs3105_picture_size(uh, page, side, &width, &height, requestsense)) {
        report("Error getting page size", requestsense);
        return 2;
      }

      int outfd;
      char *output_filename;
      if (output_to_stdout) {
        outfd = 1;
        output_filename = strdup("stdout");
      } else {
        if (asprintf(&output_filename, "%s-%03d-%s.jpeg", filebase,
                     pageno + page, side ? "B" : "A" ) == -1) {
          fprintf(stderr, "Memory allocation failed!\n");
          exit(1);
        }
        outfd = open(output_filename,
                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
      }
      if (outfd < 0) {
        fprintf(stderr, "Failed to write to %s: %s\n", output_filename,
                strerror(errno));
        free(output_filename);
        return 2;
      }
      int waitstatus;
      if ((waitstatus = kvs3105_data_buffer_wait(uh, requestsense))) {
        report("Error waiting for image data", requestsense);
        // TODO(dgluss): 3 is a bad name for a condition.  Put in a name.
        if (side == 0 && waitstatus == 3)
          fprintf(stderr, "end of book.\n");
        close(outfd);
        unlink(output_filename);
        free(output_filename);
        return 2;
      }

      uint8_t buffer[KVS3105_BUFFER_SIZE];
      unsigned done = 0;
      unsigned written;
      char end_of_page;

      for (;;) {
        if (kvs3105_read_data(uh, page, side, buffer, sizeof(buffer),
                               &written, &end_of_page, requestsense)) {
          report("Error reading image", requestsense);
          free(output_filename);
          return 2;
        }

        written = write(outfd, buffer, written);
        done += written;
        if (end_of_page) break;
      }
      fprintf(stderr, "%s: %d bytes\n", output_filename, done);
      free(output_filename);

      close(outfd);
      if (duplex) {
        if (side) {
          page++;
          side = 0;
        } else {
          side = 1;
        }
      } else {
        page++;
      }
    }
    pageno += block_size;
  }
}
