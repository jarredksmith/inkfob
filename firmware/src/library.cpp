#include "library.h"

#include <string.h>

namespace Library {

// Sizes of the fixed records in the file (see tools/build_library.py).
static const size_t HEADER_SIZE = 32;
static const size_t BOOK_SIZE = 8;
static const size_t GROUP_SIZE = 8;
static const size_t ITEM_SIZE = 12;
static const size_t IMAGE_SIZE = 24;

static const uint8_t* g_data = nullptr;
static uint16_t g_books = 0, g_groups = 0, g_items = 0, g_images = 0;
static uint32_t g_pages = 0, g_stringsSize = 0, g_textSize = 0, g_imageDataSize = 0;
static const uint8_t* g_bookRecs = nullptr;
static const uint8_t* g_groupRecs = nullptr;
static const uint8_t* g_itemRecs = nullptr;
static const uint8_t* g_pageOffsets = nullptr;
static const char* g_strings = nullptr;
static const char* g_text = nullptr;
static const uint8_t* g_imageRecs = nullptr;
static const uint8_t* g_imageData = nullptr;

// The data is not guaranteed to be aligned, so values are copied out.
static uint16_t u16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

static uint32_t u32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

bool begin(const uint8_t* data, size_t size) {
  g_data = nullptr;
  if (!data || size < HEADER_SIZE || memcmp(data, "PGLB", 4) != 0 || u16(data + 4) != 3) return false;
  g_books = u16(data + 6);
  g_groups = u16(data + 8);
  g_items = u16(data + 10);
  g_images = u16(data + 12);
  g_pages = u32(data + 16);
  g_stringsSize = u32(data + 20);
  g_textSize = u32(data + 24);
  g_imageDataSize = u32(data + 28);

  size_t pos = HEADER_SIZE;
  g_bookRecs = data + pos;
  pos += BOOK_SIZE * g_books;
  g_groupRecs = data + pos;
  pos += GROUP_SIZE * g_groups;
  g_itemRecs = data + pos;
  pos += ITEM_SIZE * g_items;
  g_pageOffsets = data + pos;
  pos += 4 * ((size_t)g_pages + 1);
  g_imageRecs = data + pos;
  pos += IMAGE_SIZE * g_images;
  g_strings = (const char*)(data + pos);
  pos += g_stringsSize;
  g_text = (const char*)(data + pos);
  pos += g_textSize;
  g_imageData = data + pos;
  pos += g_imageDataSize;
  if (pos != size) return false;
  g_data = data;
  return true;
}

bool ready() { return g_data != nullptr; }

static const char* stringAt(uint32_t offset) {
  return (g_data && offset < g_stringsSize) ? g_strings + offset : "";
}

int bookCount() { return g_data ? g_books : 0; }
const char* bookTitle(int b) { return (b >= 0 && b < bookCount()) ? stringAt(u32(g_bookRecs + BOOK_SIZE * b)) : ""; }
int bookFirstGroup(int b) { return (b >= 0 && b < bookCount()) ? u16(g_bookRecs + BOOK_SIZE * b + 4) : 0; }
int bookGroupCount(int b) { return (b >= 0 && b < bookCount()) ? u16(g_bookRecs + BOOK_SIZE * b + 6) : 0; }

int groupCount() { return g_data ? g_groups : 0; }
const char* groupTitle(int g) { return (g >= 0 && g < groupCount()) ? stringAt(u32(g_groupRecs + GROUP_SIZE * g)) : ""; }
int groupFirstItem(int g) { return (g >= 0 && g < groupCount()) ? u16(g_groupRecs + GROUP_SIZE * g + 4) : 0; }
int groupItemCount(int g) { return (g >= 0 && g < groupCount()) ? u16(g_groupRecs + GROUP_SIZE * g + 6) : 0; }

int itemCount() { return g_data ? g_items : 0; }
const char* itemTitle(int i) { return (i >= 0 && i < itemCount()) ? stringAt(u32(g_itemRecs + ITEM_SIZE * i)) : ""; }
int itemPageCount(int i) { return (i >= 0 && i < itemCount()) ? u16(g_itemRecs + ITEM_SIZE * i + 8) : 0; }

int readPage(int item, int page, char lines[][COLS + 1], int maxLines) {
  if (item < 0 || item >= itemCount() || page < 0 || page >= itemPageCount(item)) return 0;
  uint32_t index = u32(g_itemRecs + ITEM_SIZE * item + 4) + (uint32_t)page;
  if (index >= g_pages) return 0;
  uint32_t start = u32(g_pageOffsets + 4 * (size_t)index);
  uint32_t end = u32(g_pageOffsets + 4 * ((size_t)index + 1));
  if (end > g_textSize || start > end) return 0;

  int count = 0;
  int col = 0;
  for (uint32_t p = start; p < end && count < maxLines; p++) {
    char c = g_text[p];
    if (c == '\n') {
      lines[count][col] = '\0';
      count++;
      col = 0;
    } else if (col < COLS) {
      lines[count][col++] = c;
    }
  }
  if (col > 0 && count < maxLines) {  // last line without a newline
    lines[count][col] = '\0';
    count++;
  }
  return count;
}

int parseImageLine(const char* line, const char** caption) {
  if (!line || strncmp(line, "@IMG:", 5) != 0) return -1;
  int number = 0;
  const char* p = line + 5;
  if (*p < '0' || *p > '9') return -1;
  while (*p >= '0' && *p <= '9') {
    number = number * 10 + (*p - '0');
    if (number > 65535) return -1;
    p++;
  }
  if (*p != ':' || number >= imageCount()) return -1;
  if (caption) *caption = p + 1;
  return number;
}

int imageCount() { return g_data ? g_images : 0; }

bool imageBitmap(int image, bool zoom, Bitmap& out) {
  if (image < 0 || image >= imageCount()) return false;
  const uint8_t* rec = g_imageRecs + IMAGE_SIZE * image + (zoom ? 12 : 0);
  uint32_t offset = u32(rec);
  out.packedSize = u32(rec + 4);
  out.width = u16(rec + 8);
  out.height = u16(rec + 10);
  if (out.width == 0 || out.height == 0 || out.packedSize == 0) return false;
  if (offset > g_imageDataSize || out.packedSize > g_imageDataSize - offset) return false;
  out.packed = g_imageData + offset;
  return true;
}

}  // namespace Library
