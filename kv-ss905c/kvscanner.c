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

// This code is an example of how to use the kvss905c library.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>

#include <stdint.h>

#include "kvss905c.h"

int usage(const char *argv0) {
  fprintf(stderr, "Usage: %s [options] filebase\n"
                  "  -d <device>: open this device\n"
                  "  -n <number of pages to scan>\n"
                  "  -b <block size>\n"
                  "  -w <width in inches>\n"
                  "  -h <height in inches>\n"
                  "  --duplex: scan front and back\n", argv0);
  return 1;
}

int
main(int argc, char **argv) {
  const char *device = NULL;
  float width = 8.5, height = 11.0;
  unsigned num_pages = 1;
  unsigned block_size = 1;
  char duplex = 0;

  for (unsigned i = 1; i < argc - 1; ++i) {
    if (strcmp(argv[i], "--duplex") == 0) {
      duplex = 1;
      continue;
    }

    if (i == argc - 2) return usage(argv[0]);

    if (strcmp(argv[i], "-d") == 0) {
      device = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-n") == 0) {
      num_pages = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-b") == 0) {
      block_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-w") == 0) {
      width = strtof(argv[++i], NULL);
    } else if (strcmp(argv[i], "-h") == 0) {
      height = strtof(argv[++i], NULL);
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return usage(argv[0]);
    }
  }

  if (argc - 1 == 0) return usage(argv[0]);

  const char *const filebase = argv[argc - 1];

  int fd = -1;
  if (device) {
    fd = open(device, O_RDWR);
  } else {
    fd = kvss905c_open();
  }
  if (fd < 0) {
    fprintf(stderr, "Cannot open scanner\n");
    return 2;
  }

  struct kvss905c_window window;
  kvss905c_window_init(&window);

  window.document_length = window.length = height * 1200;
  window.document_width = window.width = width * 1200;

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

  uint8_t requestsense[KVSS905C_REQUEST_SENSE_SIZE];

  for (unsigned pageno = 0; pageno < num_pages;) {
    if (kvss905c_set_windows(fd, &window, duplex, requestsense)) {
      fprintf(stderr, "Error setting windows: %x\n",
              (int) scsi_error_code(requestsense));
      return 2;
    }

    if (kvss905c_scan(fd, requestsense)) {
      fprintf(stderr, "Error starting scanning: %x\n",
              (int) scsi_error_code(requestsense));
      return 2;
    }
    // We scan in blocks of block_size pages
    int side = 0;
    for (unsigned page = 0; page < block_size;) {
      uint32_t width, height;
      if (kvss905c_picture_size(fd, page, side, &width, &height, requestsense)) {
        fprintf(stderr, "Error getting page size: %x\n",
                (int) scsi_error_code(requestsense));
        return 2;
      }

      char *output_filename;
      asprintf(&output_filename, "%s-%s-%d.jpeg", filebase,
               side ? "back" : "front", pageno + page);
      const int outfd = open(output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (outfd < 0) {
        fprintf(stderr, "Failed to write to %s: %s\n", output_filename,
                strerror(errno));
        return 2;
      }

      if (kvss905c_data_buffer_wait(fd, requestsense)) {
          fprintf(stderr, "Error waiting for image data: %x\n",
                  (int) scsi_error_code(requestsense));
          return 2;
      }

      uint8_t buffer[KVSS905C_BUFFER_SIZE];
      unsigned done = 0;
      unsigned written;
      char eof;

      for (;;) {
        if (kvss905c_read_data(fd, page, side, buffer, sizeof(buffer),
                               &written, &eof, requestsense)) {
          fprintf(stderr, "Error reading image: %x\n",
                  (int) scsi_error_code(requestsense));
          return 2;
        }

        write(outfd, buffer, written);
        done += written;
        if (eof) break;
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
