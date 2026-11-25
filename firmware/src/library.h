// Read-only access to the reading library built by tools/build_library.py.
// Three levels: book > section > entry, and each entry is a run of pages of
// pre-wrapped text. No Arduino dependencies, so it can be tested on a computer.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Library {

const int COLS = 40;            // characters per line
const int LINES_PER_PAGE = 10;

// Points the reader at the packed data. Returns false if it is not valid.
bool begin(const uint8_t* data, size_t size);
bool ready();

int bookCount();
const char* bookTitle(int book);
int bookFirstGroup(int book);
int bookGroupCount(int book);

int groupCount();
const char* groupTitle(int group);
int groupFirstItem(int group);
int groupItemCount(int group);

int itemCount();
const char* itemTitle(int item);
int itemPageCount(int item);

// Copies one page of an entry into `lines` (each a NUL-terminated string).
// Returns the number of lines on the page.
int readPage(int item, int page, char lines[][COLS + 1], int maxLines);

// A page that holds a drawing has one line: "@IMG:<number>:<caption>".
// Returns the drawing number and points `caption` at its text, or -1.
int parseImageLine(const char* line, const char** caption);

// A drawing comes in two sizes: one that fits the screen width, and for most
// a larger one to zoom into. The pixels are a zlib stream which unpacks to
// `height` rows of (width + 7) / 8 bytes, 1 bit per pixel, MSB first, 1 = black.
struct Bitmap {
  const uint8_t* packed;
  uint32_t packedSize;
  uint16_t width;
  uint16_t height;
};

int imageCount();
bool imageBitmap(int image, bool zoom, Bitmap& out);  // false if there is no such version

}  // namespace Library
