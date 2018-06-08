// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dgluss@google.com (David Gluss)
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

// Interactive debugging tool for debugging wierd USB problems.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "kvs3105usb.h"
#include <libusb-1.0/libusb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

extern void report(const char *comment, uint8_t *requestsense);

typedef enum {
  CMD_NONE = 0,
  CMD_IN = 0x81,                /* scanner to pc */
  CMD_OUT = 0x02                /* pc to scanner */
} CMD_DIRECTION;                /* equals to endpoint address */

char cmdstr[256];
char *p, *q, *cmd;
char *savedptr;

struct globalstruct {
  usb_handle handle;
  struct kvs3105_window window;
  int page;
  int scanner_page;
};
static struct globalstruct g;

static void quit(char *param) {
  exit(0);
}

static void list(char *param) {
  fprintf(stdout, "%s", list_3105_devices());
}

static void usbclose(char *param) {
  if (!g.handle) {
    printf("already closed\n");
    return;
  }
  libusb_close(g.handle);
  libusb_release_interface(g.handle, 0);
  g.handle = NULL;
}

static void readside1(char *param) {
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  uint8_t buffer[KVS3105_BUFFER_SIZE];
  unsigned written;
  char end_of_page;
  for (;;) {
    if (kvs3105_read_data(g.handle, g.scanner_page, 1, buffer, sizeof(buffer),
                          &written, &end_of_page, requestsense)) {
      report("Error reading image, side 0", requestsense);
      return;
    }
    if (end_of_page) break;
  }
}

static void attach(char *param) {
  g.handle = kvs3105_open(param);
  if (!g.handle) {
    printf("didn't open\n");
    return;
  }
  kvs3105_window_init(&g.window);

  g.window.document_length = g.window.length = 11.0 * 1200;
  g.window.document_width = g.window.width = 8.5 * 1200;
  g.window.compression_argument = 90;

  // match the behavior of sheetfed_server
  g.window.emphasis = 0xf0;
  g.window.subsample = 0;
  g.window.xres = g.window.yres = 400;
  g.window.number_of_pages_to_scan = 0xff;
}

static void readpages(char *param) {
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  g.scanner_page = 0;
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  g.window.number_of_pages_to_scan = 255;
  kvs3105_reset_windows(g.handle, requestsense);
  if (kvs3105_set_windows(g.handle, &g.window,
                          1, requestsense)) {
    report("Error setting windows", requestsense);
    return;
  }
  if (kvs3105_scan(g.handle, requestsense)) {
    report("Error starting scanning", requestsense);
    return;
  }
  while (1) {
    uint32_t width, height;
    if (kvs3105_picture_size(g.handle, g.scanner_page, 0,
                             &width, &height, requestsense)) {
      report("Error getting page size, side 0", requestsense);
      return;
    }

    if (kvs3105_data_buffer_wait(g.handle, requestsense)) {
      report("Error waiting for image data, side 0", requestsense);
      return;
    }
    uint8_t buffer[KVS3105_BUFFER_SIZE];
    unsigned written;
    char end_of_page;
    char output_filename[32];
    sprintf(output_filename, "out-%d-A.jpeg", g.page);
    int outfd = open(output_filename,
                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
      printf("Failed to open %s for writing: %s\n", output_filename,
             strerror(errno));
      return;
    }
    for (;;) {
      if (kvs3105_read_data(g.handle, g.scanner_page, 0, buffer, sizeof(buffer),
                            &written, &end_of_page, requestsense)) {
        report("Error reading image, side 0", requestsense);
        write(outfd, buffer, written);
        return;
      }
      write(outfd, buffer, written);
      if (end_of_page) break;
    }
    close(outfd);
    printf("read side 0\n");
    if (kvs3105_picture_size(g.handle, g.scanner_page, 1,
                             &width, &height, requestsense)) {
      report("Error getting page size, side 1", requestsense);
      return;
    }
    if (kvs3105_data_buffer_wait(g.handle, requestsense)) {
      report("Error waiting for image data, side 1", requestsense);
      return;
    }
    sprintf(output_filename, "out-%d-B.jpeg", g.page);
    outfd = open(output_filename,
                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
      printf("Failed to open %s for writing: %s\n", output_filename,
             strerror(errno));
      return;
    }
    for (;;) {
      if (kvs3105_read_data(g.handle, g.scanner_page, 1, buffer, sizeof(buffer),
                            &written, &end_of_page, requestsense)) {
        report("Error reading image, side 1", requestsense);
        write(outfd, buffer, written);
        return;
      }
      write(outfd, buffer, written);
      if (end_of_page) break;
    }
    close(outfd);
    printf("read side 1\n");
    printf("read page %d\n", g.page);
    g.scanner_page++;
    g.page++;
    // The scanner only understands page numbers from 0 to 255. We have
    // to ask for 0 after we've gotten 255.
    if (g.scanner_page > 255)
      g.scanner_page = 0;
  }
}

static void windows_reset(char *param) {
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  kvs3105_reset_windows(g.handle, requestsense);
}

static void read_one(char *param) {
  uint8_t requestsense[KVS3105_REQUEST_SENSE_SIZE];
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  kvs3105_window_init(&g.window);

  g.window.document_length = g.window.length = 11 * 1200;
  g.window.document_width = g.window.width = 8.5 * 1200;
  g.window.compression_argument = 90;

  // match the behavior of sheetfed_server
  g.window.emphasis = 0xf0;
  g.window.subsample = 0;
  g.window.xres = g.window.yres = 400;

  g.window.number_of_pages_to_scan = 1;

  if (kvs3105_set_windows(g.handle, &g.window,
                          1, requestsense)) {
    report("Error setting windows", requestsense);
    return;
  }
  if (kvs3105_scan(g.handle, requestsense)) {
    report("Error starting scanning", requestsense);
    return;
  }
  uint32_t width, height;
  if (kvs3105_picture_size(g.handle, 0, 0,
                           &width, &height, requestsense)) {
    report("Error getting page size", requestsense);
    return;
  }

  if (kvs3105_data_buffer_wait(g.handle, requestsense)) {
    report("Error waiting for image data", requestsense);
    return;
  }
  uint8_t buffer[KVS3105_BUFFER_SIZE];
  unsigned written;
  char end_of_page;

  for (;;) {
    if (kvs3105_read_data(g.handle, 0, 0, buffer, sizeof(buffer),
                          &written, &end_of_page, requestsense)) {
      report("Error reading image", requestsense);
      return;
    }
    if (end_of_page) break;
  }
  printf("one page scanned.\n");
}

static void clearboth(char *param) {
  if (!g.handle) {
    g.handle = kvs3105_open(param);
    if (!g.handle) {
      printf("didn't open\n");
      return;
    }
  }
  libusb_clear_halt(g.handle, CMD_IN);
  libusb_clear_halt(g.handle, CMD_OUT);
}

static void ci(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  libusb_clear_halt(g.handle, CMD_IN);
}

static void co(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  libusb_clear_halt(g.handle, CMD_OUT);
}

static int find_and_open(char *param) {
  if (g.handle)
    return 1;
  /* Use libusb */
  libusb_init(0);
  // discover devices
  libusb_device **device_list;
  libusb_device *found = NULL;
  ssize_t cnt;
  int seekbus = -1, seekdev = -1;
  if (param && *param)
    sscanf(param, "%d:%d", &seekbus, &seekdev);
  cnt = libusb_get_device_list(NULL, &device_list);
  int err = 0;
  if (cnt < 0) {
    printf("No USB devices of any sort found!\n");
    return 0;
  }
  for (int i = 0; i < cnt; i++) {
    libusb_device *device = device_list[i];
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor (device, &desc);
    if (desc.idVendor == KVS3105_VENDOR_ID &&
        desc.idProduct == KVS3105_ID) {
      if (seekbus >= 0 &&
          (seekbus != libusb_get_bus_number(device) ||
           seekdev != libusb_get_device_address(device)))
        continue;
      printf("found %d:%d\n",
             libusb_get_bus_number(device),
             libusb_get_device_address(device));
      found = device;
      break;
    }
  }
  if (found) {
    err = libusb_open(found, &g.handle);
    if (err) {
      found = NULL;
    }
  }
  libusb_free_device_list(device_list, 1);
  if (!found) {
    printf("no device found\n");
    return 0;
  }
  return 1;
}
static void reset(char *param) {
  if (!find_and_open(param)) {
    printf("didn't open\n");
    return;
  }
  libusb_reset_device(g.handle);
  libusb_release_interface(g.handle, 0);
  g.handle = NULL;
  libusb_exit(0);
}

static void reset_device(char *param) {
  if (!find_and_open(param)) {
    printf("didn't open\n");
    return;
  }
  libusb_reset_device(g.handle);
}

static void claim(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  libusb_claim_interface(g.handle, 0);
}

static void release(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  libusb_release_interface(g.handle, 0);
}

static void config(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  libusb_set_configuration(g.handle, 0);
  libusb_set_configuration(g.handle, 1);
}

extern int kvs3105_unit_not_ready(usb_handle uh);
static void testready(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  printf("kvs3105_unit_not_ready returned %d\n",
         kvs3105_unit_not_ready(g.handle));
}

static void detect(char *param) {
  if (!g.handle) {
    printf("attach first.\n");
    return;
  }
  kvs3105_detect(g.handle);
}

struct cmdtable {
  const char *cmdname;
  void (*func)(char *);
  const char *helptext;
} cmds[] = {
  // Sorted would be nice.
  {"attach", attach, 0},
  {"ci", ci, "usb clear halt on input channel"},
  {"claim", claim, 0},
  {"clear", clearboth, "usb clear halt on both channels"},
  {"close", usbclose, 0},
  {"co", co, "usb clear halt on output channel"},
  {"config", config, 0},
  {"detect", detect, 0},
  {"list", list, 0},
  {"quit", quit, 0},
  {"r1", read_one, "read one page"},
  {"rd", reset_device, "reset device"},
  {"read", readpages, "read a book"},
  {"read1", read_one, "read one page"},
  {"readside1", readside1, 0},
  {"release", release, 0},
  {"reset", reset, "reset the device and interface and detach"},
  {"resetdevice", reset_device, 0},
  {"rs1", readside1, "read side 1 only"},
  {"testready", testready, "test usb unit ready"},
  {"windows_reset", windows_reset, "set window with empty data"},
  {0},
};

void do_interactive() {
  int count, len, i, cmdi;
  g.page = 0;
  while (1) {
    fputs("> ", stdout);
    fgets(cmdstr, sizeof(cmdstr), stdin);
    for (p = cmdstr; *p && isspace(*p); p++);
    if (!*p)
      continue;
    cmd = strtok(p, " \t\n");
    count = 0;
    len = strlen(cmd);
    cmdi = -1;
    for (i = 0; cmds[i].cmdname; i++) {
      if (!strncmp(cmds[i].cmdname, cmd, len)) {
        count++;
        cmdi = i;
      }
      if (!strcmp(cmds[i].cmdname, cmd)) {
        count = 1;
        cmdi = i;
        break;
      }
    }
    if (count == 1) {
      (*cmds[cmdi].func)(strtok(NULL, "\n"));
      continue;
    }
    if (count > 1) {
      printf("command %s matches:\n", cmd);
      for (i = 0; cmds[i].cmdname; i++) {
        if (!strncmp(cmds[i].cmdname, cmd, len))
          printf("  %s\n", cmds[i].cmdname);
      }
      continue;
    }
    printf("command %s not found, try one of:\n", cmd);
    for (i = 0; cmds[i].cmdname; i++) {
      printf("  %s", cmds[i].cmdname);
      if (cmds[i].helptext)
        printf("  (%s)", cmds[i].helptext);
      putchar('\n');
    }
  }
}
