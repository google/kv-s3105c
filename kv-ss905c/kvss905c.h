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



// Welcome, gentle reader. This is a very simple *userspace* driver for a
// Panasonic KV-SS905C scanner:
//   http://panasonic.co.jp/pcc/products/en/scanner/ss905c_e.htm
//
// This is a high-speed, bulk document scanner and the following provides a
// minimal interface to it. It was written with reference to
//   KV-S3105CS3085_if-e.pdf 05221d89ea5eb7b25ae98f903a559d07
//
// This code talks to the scanner via a SCSI-Generic device (/dev/sg*). The
// mode of operation is thus:
//
// Sequence of Calls
// -----------------
//
// 1) Open the scanner.
//
// This involves finding the device to open and opening it O_RDWR. You can use
// kvss905c_detect to test if an open handle looks like a good candidate or
// kvss905c_open to search all SCSI generic devices for a good looking one.
//
// 2) Setup the window
//
// The window contains all the scanning settings. There's a commented structure
// below which describes the settings. The comments are taken from the above
// referenced document and aren't always as complete as I would like. However,
// kvss905c_window_init will give you good default settings. The fields you
// should look at changing are:
//   width, document_width, height, document_height: scanning size
//   composition, bpp: selecting grayscale/colour etc
//   compression_*
//   number_of_pages_to_scan
//
// 3) Set the window
//
// Once you have setup the window structure, call kvss905c_set_windows
// 
// 3) Start the scan
//
// The scanner is designed to scan many pages of a document and so it can scan
// between 1..254 pages in a pipeline, or it can be in continuous mode, which
// scans until it runs out of paper (number_of_pages_to_scan = 255).
// 
// A scan scan's the given number of pages and is started by calling
// kvss905c_scan. This can return error 0x3a00 if there's no paper. (See
// function conventions, below)
//
// 4) For each page...
//   5) For each side...
//     6) Wait for the image with kvss905c_data_buffer_wait
//     7) Get the size of the page with kvss905c_picture_size
//     8) Get the image data with kvss905c_read_data. If you run out of paper
//        at this point, the scanner will beep and you'll get error 0x3a00.
//
// Note that you must read the pages in order (0, 1, 2, 3 ...) and, if you are
// scanning duplex, you must read the front before the back. If you don't,
// you'll get error 0x2400.
//
// If you were scanning a fixed number of pages and you wish to scan more, you
// can go back to step 3.
//
// This code doesn't use the mmap interface to read the data from the SCSI bus
// because the data rates (about 15MB/s when reading raw data) don't really
// justify it.
//
// Function Conventions
// --------------------
//
// Funtions, unless the return value is specified in the comments, are assumed
// to return 0 on success and non-zero on error. Many also take an argument
// called requestsense. requestsense must be a writable buffer of length
// KVSS905C_REQUEST_SENSE_SIZE.
//
// If one of these functions returns non-zero you can get error information
// from the requestsense buffer. Two of the bytes are the SCSI ASC/ASCQ numbers
// (see http://www.t10.org/lists/asc-num.htm) and you can extract this as a u32
// via scsi_error_code.
//
// The KV-* series scanners have some of their own errors in addition to the
// standard SCSI ones. You can translate to a human-readable string by
// kvss905c_strerror
// -----------------------------------------------------------------------------

#ifndef _KVSS905C_INCLUDE_GUARD
#define _KVSS905C_INCLUDE_GUARD

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// These are the scanning modes supported by the scanner
#define KVSS905C_COMPOSITION_BINARY 0
#define KVSS905C_COMPOSITION_GRAYSCALE 2
#define KVSS905C_COMPOSITION_COLOUR 5

#define KVSS905C_REQUEST_SENSE_SIZE 20
#define KVSS905C_BUFFER_SIZE 0x10000

uint16_t scsi_error_code(const uint8_t *requestsense);

// -----------------------------------------------------------------------------
// Search for the first likely looking compatible scanner and return a file
// descriptor to it. Otherwise, return -1 if none could be found
// -----------------------------------------------------------------------------
int kvss905c_open();

// -----------------------------------------------------------------------------
// Return 0 if the SCSI generic device designated by fd appears to be a
// Panasonic KV series scanner
// -----------------------------------------------------------------------------
int kvss905c_detect(int fd);

// -----------------------------------------------------------------------------
// Start scanning
//   fd: open scanner device
// -----------------------------------------------------------------------------
int kvss905c_scan(int fd, uint8_t *requestsense);

// -----------------------------------------------------------------------------
// Stop a running scan. You can continue to read pages from the scanner, but at
// some point you will get an ADF stopped error (0x8002)
//   fd: open scanner device
// -----------------------------------------------------------------------------
int kvss905c_stop(int fd, uint8_t *requestsense);

// -----------------------------------------------------------------------------
// Get the size of a scanned image
//   fd: open scanner device
//   page: page number
//   back: if non-zero, get the back of the page (for duplex mode)
//   width, height: (output, may be NULL) the dimention in pixels
// -----------------------------------------------------------------------------
int kvss905c_picture_size(int fd, uint8_t page, uint8_t back, uint32_t *width,
                          uint32_t *height, uint8_t *requestsense);

// -----------------------------------------------------------------------------
// Read a scanned image
//   fd: open scanner device
//   page: page number
//   back: if non-zero, get the back of the page (for duplex mode)
//   buffer: the buffer to write to
//   length: maximum amount of data to write
//   result: (output, non-NULL) the number of bytes written to buffer
//   eof: (output, non-NULL) if non-zero, the last byte of image data is in
//        buffer
// -----------------------------------------------------------------------------
int kvss905c_read_data(int fd, uint8_t page, uint8_t back, uint8_t *buffer,
                       unsigned length, unsigned *result, char *eof,
                       uint8_t *requestsense);


// -----------------------------------------------------------------------------
// Return a human readable (English) string describing the error, or NULL if
// the error is unknown.
// -----------------------------------------------------------------------------
const char *
kvss905c_strerror(uint8_t *requestsense);

// -----------------------------------------------------------------------------
// Wait for the image buffer to be ready. Call this before reading every image.
//   fd: an open scanner
// -----------------------------------------------------------------------------
int kvss905c_data_buffer_wait(int fd, uint8_t *requestsense);

// -----------------------------------------------------------------------------
// This structure describes the scanning setup. This includes both the standard
// SCSI fields and the device-specific ones. The comments are taken from the
// documentation of the SET WINDOW command.
//
// A window can be set for the front and back (for duplex scanning) and they
// need not be the same. This code doesn't handle that, however.
// -----------------------------------------------------------------------------
struct kvss905c_window {
  // DPI resolution for X and Y directions
  //   default: 0 = 400 dpi
  uint16_t xres, yres;
  // The origin and page size (see also document_size, below)
  // Units are 1/1200 of an inch
  uint32_t x1, y1, width, length;
  // brighness: 0 -> normal
  //            1 -> lightest
  //            ...
  //            0xff -> darkest
  // threshold: valid if composition is binary
  //   0 -> 0x80
  //   1 -> lightest
  //   0xff -> darkest
  // contrast:
  //   0 -> 0x80
  //   1 -> lowest
  //   0xff -> highest
  // composition:
  //   KVSS905C_COMPOSITION_BINARY
  //   KVSS905C_COMPOSITION_GRAYSCALE
  //   KVSS905C_COMPOSITION_COLOUR
  // bpp:
  //   1 (for binary)
  //   8 (for grayscale)
  //   24 (colour)
  uint8_t brighness, threshold, contrast, composition, bpp;
  // documents suggest that this isn't actually supported
  uint16_t halftone_pattern;
  // if it's a binary image:
  //   0 -> no transformation (default)
  //   1 -> invert image
  uint8_t reverse_image;
  // If compression isn't being used:
  //   0 -> LSB first
  //   1 -> MSB first
  uint16_t bit_ordering;
  // compression_type:
  //   0 -> none
  //   1 -> MH (group 3)
  //   2 -> MR (group 3)
  //   3 -> MMR (group 4)
  //   4 -> JPEG
  // compression_argument:
  //   if compression_type == 2 -> K parameter
  //   if compression_type == 4 -> JPEG quality (1..100)
  uint8_t compression_type, compression_argument;

  // ignore
  uint8_t flatbed;
  // if true, stop if the paper is skewed
  uint8_t stop_on_skew;
  uint8_t disable_buffering;
  uint8_t continue_on_double_feed;
  // 0 -> no transformation
  // 0x80 -> left-right mirror
  uint8_t mirror_image;
  // Documents aren't clear what this actually does
  //   1..0x2f -> medium (default)
  //   0x30..0x4f -> high
  //   0x50..0x7f -> high
  //   0x80..0xff -> high
  uint8_t emphasis;
  // If grayscale:
  //   0 -> normal
  //   1 -> CRT
  //   2 -> linear
  //   0x10 -> convert to binary
  //   0x11 -> convert to binary for CRT
  //   0x20 -> convert to binary dither
  //   0x21 -> convert to binary dither for CRT
  //   0x30 -> convert to binary error diffusion
  //   0x31 -> convert to binary error diffusion for CRT
  //   0x80 -> use downloaded gamma tables
  uint8_t gamma_correction;
  // If 1, the color for the drop out is given with a SEND command and the lamp
  // parameter (next) is ignored
  uint8_t multi_colour_drop_out;
  // 0 -> white (default)
  // 1 -> red
  // 2 -> green
  // 3 -> blue
  uint8_t lamp;
  // 0 -> normal
  // 1 -> high
  // 2 -> low
  uint8_t double_feed_sensitivity;
  // 0 -> do not remove
  // 1 -> remove
  uint8_t remove_moire;
  // 0 -> 4:4:4
  // 1 -> 4:1:1
  // 2 -> 4:2:0 (not supported)
  // 3 -> 4:2:2
  uint8_t subsample;
  // If set, disables gamma correction
  //   0 -> disabled
  //   1 -> output with sRGB
  // see http://en.wikipedia.org/wiki/SRGB_color_space
  uint8_t colour_match;
  // bit 7: Standard document size (bits 4..0 effective)
  // bit 6: stop scanning the current page if the paper is shorter than the
  //        given length
  // bit 5: long paper mode: read a long page by splitting it up into length
  //        spans
  // bit 4:
  //   0 -> portrait
  //   1 -> landscape
  // bits 0..3:
  //   0 -> auto detect (doesn't appear to work)
  //   1 -> 2.2x3.6"
  //   3 -> A3
  //   4 -> A4
  //   5 -> A5
  //   6 -> A6
  //   7 -> letter
  //   9 -> double letter
  //   0xc -> b4
  //   0xd -> b5
  //   0xe -> b6
  //   0xf -> legal
  uint8_t document_size;
  // in units of 1/1200 of an inch
  // only effective if Standard document size isn't set
  uint32_t document_width, document_length;
  uint8_t ahead_disable;
  // 0 -> disabled
  // 1 -> detect but don't correct (can be obtained with a READ command)
  // 2 -> correct
  uint8_t deskew;
  uint8_t double_feed_detector;
  // if set, scan the whole page and scale to the requested size
  uint8_t full_size_scan;
  uint8_t feed_slow;
  uint8_t remove_shadow;
  // number of pages to scan in one block
  //  0 -> 1 page
  //  1 -> 1 page
  //  2 -> 2 pages
  //  ...
  //  0xfe -> 254 pages
  //  0xff -> all pages
  uint8_t number_of_pages_to_scan;
  // if binary:
  //   0 -> static threshold mode
  //   0x11 -> dynamic threshold (lightest)
  //   ...
  //   0x1f -> dynamic threshold (darkest)
  uint8_t threshold_mode;
  // ignore
  uint8_t separation_mode;
  uint8_t standard_white_level;
  uint8_t blackwhite_noise_reduction;
  uint8_t noise_reduction;
  uint8_t manual_feed_mode;
  // the following aren't actully supported on the KVSS905C
  uint8_t additional_space_top;
  uint8_t additional_space_bottom;
  // these are...
  uint8_t detect_separation_sheet;
  uint8_t halt_at_separation_sheet;
  uint8_t detect_control_sheet;
  uint8_t stop_mode;
  // .. and these aren't
  uint8_t red_chroma;
  uint8_t blue_chroma;
};

// -----------------------------------------------------------------------------
// Set sensible default values for all the fields in the window.
// -----------------------------------------------------------------------------
void kvss905c_window_init(struct kvss905c_window *window);

// -----------------------------------------------------------------------------
// Setup the scanning by setting windows:
//   fd: open scanner device
//   duplex: if non-zero, setup duplex scanning using the same settings for
//           front and back
// -----------------------------------------------------------------------------
int kvss905c_set_windows(int fd, const struct kvss905c_window *window,
                         char duplex, uint8_t *requestsense);


#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // _KVSS905C_INCLUDE_GUARD
