// iPhone notification pager for the Elecrow CrowPanel 2.13" e-ink board.
//
// The board pairs with an iPhone as a Bluetooth accessory and reads the
// phone's notifications through Apple's Notification Center Service (ANCS).
//
// Controls
//   Rocker up/down   move through the list, or scroll a message
//   Rocker press     open the selected notification / go back to the list
//   EXIT             back
//   MENU             in a message: dismiss it on the phone.  In the list: settings
//
// Hold at power-on
//   MENU             switch to the other screen type (if the screen stays blank)
//   EXIT             forget the paired phone
//
// Sleep
//   After the "Sleep after" time with no button presses the pager powers
//   right down (deep sleep).  The screen keeps its picture.  Any button wakes
//   it; it can also wake by itself every so often to check the phone.

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>

#include <sys/time.h>
#include <algorithm>
#include <vector>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_system.h"

#include "miniz.h"  // zlib unpacking, built into the chip

#include "epd.h"
#include "library.h"
#include "personal.h"

// The packed reading library, embedded by src/library_blob.S.
extern "C" const uint8_t pager_library_start[];
extern "C" const uint8_t pager_library_end[];

// ------------------------------------------------------------------- pins
static const int PIN_EXIT = 1;
static const int PIN_MENU = 2;
static const int PIN_DOWN = 4;
static const int PIN_CLICK = 5;
static const int PIN_UP = 6;
static const int PIN_LED = 19;

// ------------------------------------------------------------------- ANCS
static const NimBLEUUID ANCS_SERVICE("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

// Apple Media Service: what is playing, plus play/pause/skip/volume.
static const NimBLEUUID AMS_SERVICE("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
// Standard Bluetooth services the iPhone also offers: its clock and its battery.
static const NimBLEUUID TIME_SERVICE((uint16_t)0x1805);
static const NimBLEUUID TIME_CURRENT((uint16_t)0x2A2B);
static const NimBLEUUID BATTERY_SERVICE((uint16_t)0x180F);
static const NimBLEUUID BATTERY_LEVEL((uint16_t)0x2A19);

static const uint16_t MAX_TITLE = 48;
static const uint16_t MAX_SUBTITLE = 48;
static const uint16_t MAX_MESSAGE = 600;
static const size_t MAX_NOTES = 20;

struct NsEvent {
  uint8_t eventId;
  uint8_t flags;
  uint8_t category;
  uint32_t uid;
};

struct Note {
  uint32_t uid;
  uint8_t category;
  bool unread;
  String app;
  String title;
  String body;
};

struct PendingFetch {
  uint32_t uid;
  uint8_t category;
  bool announce;     // pop it up on screen when it arrives
  bool preexisting;  // was already on the phone before the pager connected
};

static const char* const FIRMWARE_VERSION = "v2.6";

// One write to the phone's control point, carried out by the Bluetooth task.
struct BleRequest {
  uint8_t data[16];
  uint8_t len;
  uint8_t target;  // 0 = notification control point, 1 = media remote
  bool isFetch;
  uint32_t uid;
};

// Clock, battery and now-playing updates from the phone, handed to the main loop.
enum InfoKind : uint8_t { INFO_TIME, INFO_BATTERY, INFO_MEDIA };
struct InfoEvent {
  uint8_t kind;
  uint8_t a;
  uint8_t b;
  uint8_t len;
  uint8_t data[96];
};

static QueueHandle_t g_nsQueue;       // notification events from the phone
static QueueHandle_t g_requestQueue;  // writes waiting to go to the phone
static QueueHandle_t g_buttonQueue;   // button presses
static QueueHandle_t g_infoQueue;     // clock / battery / now-playing updates
static StreamBufferHandle_t g_dsStream;

static volatile bool g_connected = false;
static volatile bool g_secured = false;
static volatile uint16_t g_connHandle = 0;
static volatile uint32_t g_connGeneration = 0;  // goes up on every new connection
static volatile uint32_t g_loopGeneration = 0;  // the connection the main loop is ready for
static volatile bool g_ancsReady = false;
// Link history, shown on the Status screen to help diagnose reconnect trouble.
static volatile int g_dropReason = -1;      // why the phone link last ended
static volatile uint32_t g_dropAtMs = 0;
static volatile uint16_t g_dropCount = 0;
static volatile uint16_t g_connectCount = 0;
static volatile uint32_t g_fastAdvUntilMs = 0;  // advertise rapidly until then
static volatile bool g_advFast = false;

// A short history of phone links, for the Link log page.
struct LinkRecord {
  uint32_t endMs;     // when it dropped
  uint32_t seconds;   // how long it lasted
  uint8_t stage;      // 0 linked, 1 paired, 2 notifications working
  uint8_t reason;     // Bluetooth reason code for the drop
  uint16_t notes;     // notifications received during it
};
static const int LINK_LOG_SIZE = 6;
static LinkRecord g_linkLog[LINK_LOG_SIZE];
static volatile uint8_t g_linkLogCount = 0;   // total recorded (newest is at (count-1) % size)
static volatile uint32_t g_linkStartMs = 0;
static volatile uint8_t g_linkStage = 0;
static volatile uint16_t g_linkNotes = 0;
static int g_statusPage = 0;
static volatile uint8_t g_discoverTries = 0;
static volatile bool g_fetchFailed = false;
static volatile uint32_t g_failedUid = 0;

static std::vector<Note> g_notes;  // newest first
static std::vector<PendingFetch> g_pending;
static bool g_fetching = false;
static PendingFetch g_fetch;
static uint32_t g_fetchStartMs = 0;
static uint8_t g_rx[900];
static size_t g_rxLen = 0;

// --------------------------------------------------------------------- UI
enum Screen {
  SCREEN_HOME,
  SCREEN_MENU,
  SCREEN_LIST,
  SCREEN_DETAIL,
  SCREEN_MEDIA,
  SCREEN_LIBRARY,
  SCREEN_READER,
  SCREEN_SETTINGS,
  SCREEN_STATUS,
  SCREEN_TOOLS,
  SCREEN_TIMER,
  SCREEN_STOPWATCH,
  SCREEN_REMOTE,
  SCREEN_CARD,
  SCREEN_ORACLE,
  SCREEN_DICE,
  SCREEN_COIN,
  SCREEN_FILES,
  SCREEN_NOTE,
  SCREEN_UPDATE,
  SCREEN_SNAKE,
  SCREEN_2048,
  SCREEN_TODAY,
  SCREEN_DECK,
  SCREEN_GAMES,
  SCREEN_POMO,
  SCREEN_TALLY,
  SCREEN_TIP,
  SCREEN_CONVERT,
  SCREEN_FIND,
  SCREEN_MORSE,
  SCREEN_HANGMAN,
  SCREEN_MINES,
  SCREEN_WAYPOINTS,
  SCREEN_WAYPOINT,
  SCREEN_KNOTS,
  SCREEN_TODO,
  SCREEN_GROUP
};
static Screen g_screen = SCREEN_HOME;

// Clock: the phone's local time when it was last read, and when that was.
static bool g_timeKnown = false;
static int64_t g_timeBase = 0;  // seconds since 1970-01-01, in local time
static uint32_t g_timeBaseMs = 0;
static int g_homeMinute = -1;   // minute of the day last drawn on the home screen

static int g_phoneBattery = -1;  // percent, or -1 if unknown

static String g_mediaTitle;
static String g_mediaArtist;
static String g_mediaPlayer;
static bool g_mediaPlaying = false;
static bool g_mediaSkipMode = false;  // rocker skips tracks instead of changing volume

static Screen g_detailReturn = SCREEN_LIST;  // where EXIT goes from an opened notification
static int g_menuSel = 0;
static int g_group = 0;                 // which sub-menu is open: 0 Reading, 1 Phone
static int g_groupSel[2] = {0, 0};
static Screen g_backTo = SCREEN_TOOLS;  // where EXIT goes from a tool: the list it was opened from

// Library browser: level 0 = books, 1 = sections of a book, 2 = entries of a section.
static int g_libLevel = 0;
static int g_libBook = 0;
static int g_libGroup = 0;
static int g_libSel[3] = {0, 0, 0};
static int g_libTop[3] = {0, 0, 0};
// Reader position, and the saved "continue reading" bookmark.
static int g_readItem = 0;
static int g_readPage = 0;
static int g_markItem = -1;
static int g_markPage = 0;
static uint8_t g_turnsSinceSave = 0;

// Drawing viewer. A drawing is unpacked into g_imageBuf when its page opens.
static const size_t IMAGE_BUF_SIZE = 32768;
static uint8_t g_imageBuf[IMAGE_BUF_SIZE];
static int g_imageLoaded = -1;
static bool g_imageLoadedZoom = false;
static Library::Bitmap g_imageInfo = {};
static bool g_imageZoom = false;  // showing the larger version
static int g_imageY = 0;          // how far the view is scrolled down
static int g_imageXStep = 0;      // zoomed view: 0 left, 1 middle, 2 right
static const int IMAGE_VIEW_H = EPD_HEIGHT - 14;
static const int IMAGE_SCROLL_STEP = 80;
static int g_sel = 0;        // selected row in the list
static int g_listTop = 0;    // first visible row
static uint32_t g_detailUid = 0;
static int g_detailTop = 0;  // first visible body line
static int g_detailLines = 0;
static int g_settingsSel = 0;
static int g_settingsTop = 0;
static uint8_t g_sleepFace = 0;  // 0 = status, 1 = my card
// Home screen face: 0 classic, 1 big clock, 2 weather, 3 agenda, 4 minimal, 5 photo.
static const int FACE_COUNT = 8;
static const char* const FACE_NAMES[FACE_COUNT] = {"Classic", "Big clock", "Weather", "Agenda",
                                                   "Minimal", "Photo",     "Custom",  "To-do"};
// The custom face: what goes in each corner (top left, top right, bottom left, bottom right).
// 0 nothing, 1 date, 2 weather, 3 next event, 4 steps, 5 phone battery, 6 unread,
// 7 countdown, 8 ticker, 9 reminder.
static uint8_t g_corners[4] = {1, 2, 3, 6};
static uint8_t g_face = 0;
// The photo face: a 250 x 122 one-bit picture the app sends as "face.img".
static const size_t PHOTO_BYTES = ((EPD_WIDTH + 7) / 8) * EPD_HEIGHT;
static uint8_t g_photo[PHOTO_BYTES];
static bool g_photoTried = false;
static bool g_photoOk = false;
// Two-button control, for a pager whose wheel is stiff, buried or broken:
// tap MENU (the top button) = up, tap EXIT (the bottom one) = down, double-tap either (or press both) = select,
// hold EXIT = back, hold MENU = menu. The wheel is ignored while this is on.
static volatile bool g_twoButton = false;
static volatile bool g_repeatOk = false;   // holding the wheel up or down repeats
static volatile bool g_rendering = false;
static volatile uint32_t g_pressAtMs = 0;  // when the button being handled went down
static NimBLECharacteristic* g_hidInput = nullptr;

static bool g_dirty = true;
static uint32_t g_dirtyAtMs = 0;     // redraw no earlier than this
static uint32_t g_dirtySinceMs = 0;  // when the screen first became out of date
static bool g_dirtyUrgent = false;   // a button was pressed: redraw straight away
static uint8_t g_fastCount = 0;
static bool g_forceFull = true;
static bool g_expedition = false;            // Bluetooth off, sleeps fast, wakes on a button only
static std::vector<String> g_todo;
static std::vector<bool> g_todoDone;
static uint8_t g_readTurn = 0;               // 0 wide, 1 tall, 2 tall the other way up
static const char* const READ_NAMES[] = {"Wide", "Tall", "Tall, flipped"};

static Preferences g_prefs;
static bool g_flip = false;
static bool g_mirror = true;
static bool g_fastRefresh = true;
static bool g_smallText = true;

// Sleep settings: positions in these lists are what gets saved.
static const uint16_t SLEEP_MINUTES[] = {0, 1, 5, 15, 30, 60};  // 0 = never
static const int SLEEP_CHOICES = 6;
static const uint16_t CHECK_MINUTES[] = {0, 5, 15, 30, 60};     // 0 = do not check
static const int CHECK_CHOICES = 5;
// How long a notification that pops up by itself stays before the screen goes back.
static const uint16_t POPUP_SECONDS[] = {0, 10, 30, 60, 300};  // 0 = until dismissed
static const int POPUP_CHOICES = 5;
static uint8_t g_popupChoice = 2;
static bool g_popupActive = false;   // the notification on screen opened by itself
static uint32_t g_popupAtMs = 0;
static uint32_t g_popupUid = 0;
static uint8_t g_sleepChoice = 0;
static uint8_t g_checkChoice = 2;
static uint32_t g_lastActivityMs = 0;  // last button press or incoming notification
static bool g_fromSleep = false;       // this start-up is a wake from sleep
static bool g_quiet = false;           // woke by itself to check the phone; screen untouched
static bool g_displayStarted = false;
static int g_newSinceSleep = 0;        // notifications that arrived while asleep
static uint32_t g_readyAtMs = 0;       // when the phone started sending notifications
static uint32_t g_lastNsMs = 0;        // when the phone last mentioned a notification
static volatile bool g_sleeping = false;

// Kept alive through sleep (but not through a power cut or reset).
static const int SEEN_SLOTS = 256;
struct SleepMemory {
  uint8_t resumeReader;  // was reading when it went to sleep
  uint8_t timeValid;     // the chip's own clock was set before sleeping
  uint16_t seenCount;
  uint16_t seenNext;
  uint32_t seen[SEEN_SLOTS];  // fingerprints of notifications already shown
};
static RTC_DATA_ATTR SleepMemory g_rtc = {};

static GFXcanvas1 g_canvas(EPD_WIDTH, EPD_HEIGHT);

static const int HEADER_H = 14;

// Two text sizes: the small built-in 6x8 font, or the larger 9 point one.
static void useFont(bool bold) {
  if (g_smallText) {
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
  } else {
    g_canvas.setFont(bold ? &FreeSansBold9pt7b : &FreeSans9pt7b);
  }
}
static int lineHeight() { return g_smallText ? 10 : 18; }
static int rowHeight() { return g_smallText ? 27 : 36; }
static int rowsVisible() { return g_smallText ? 4 : 3; }
static int bodyLinesVisible() { return g_smallText ? 9 : 5; }

// Prints one line with its top edge at `top`. The small font has no bold
// version, so bold is made by printing twice, one pixel apart.
static void printLine(int x, int top, const String& text, bool bold) {
  useFont(bold);
  int y = top + (g_smallText ? 1 : 13);
  g_canvas.setCursor(x, y);
  g_canvas.print(text);
  if (g_smallText && bold) {
    g_canvas.setCursor(x + 1, y);
    g_canvas.print(text);
  }
}

static void markDirty(uint32_t delayMs = 0) {
  uint32_t at = millis() + delayMs;
  if (!g_dirty) g_dirtySinceMs = millis();
  if (delayMs == 0) g_dirtyUrgent = true;
  if (!g_dirty || (int32_t)(at - g_dirtyAtMs) < 0) g_dirtyAtMs = at;
  g_dirty = true;
}

// ------------------------------------------------------------ text helpers

// The built-in fonts only cover plain ASCII, so phone text is folded down to
// it: curly quotes and dashes are replaced, accents dropped, emoji removed.
static String toAscii(const uint8_t* data, size_t len) {
  String out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    uint8_t c = data[i];
    uint32_t cp = 0;
    int extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      i++;
      continue;
    }
    if (i + extra >= len) break;  // truncated sequence at the end
    for (int k = 1; k <= extra; k++) cp = (cp << 6) | (data[i + k] & 0x3F);
    i += extra + 1;

    if (cp == '\n' || cp == '\r') {
      out += '\n';
    } else if (cp == '\t') {
      out += ' ';
    } else if (cp >= 0x20 && cp < 0x7F) {
      out += (char)cp;
    } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) {
      out += '\'';
    } else if (cp == 0x201C || cp == 0x201D) {
      out += '"';
    } else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2010 || cp == 0x2011) {
      out += '-';
    } else if (cp == 0x2026) {
      out += "...";
    } else if (cp == 0x00A0 || cp == 0x2009 || cp == 0x202F) {
      out += ' ';
    } else if (cp == 0x2022 || cp == 0x00B7) {
      out += '*';
    } else if (cp >= 0x00C0 && cp <= 0x00FF) {
      static const char fold[] =
          "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      out += fold[cp - 0x00C0];
    }
    // everything else (emoji, other scripts, control codes) is dropped
  }
  return out;
}

static String appName(const String& bundleId) {
  static const char* const known[][2] = {
      {"com.apple.MobileSMS", "Messages"},
      {"com.apple.mobilephone", "Phone"},
      {"com.apple.mobilemail", "Mail"},
      {"com.apple.mobilecal", "Calendar"},
      {"com.apple.reminders", "Reminders"},
      {"com.apple.facetime", "FaceTime"},
      {"com.google.Gmail", "Gmail"},
      {"com.google.calendar", "Calendar"},
      {"com.microsoft.skype.teams", "Teams"},
      {"com.microsoft.Office.Outlook", "Outlook"},
      {"com.tinyspeck.chatlyio", "Slack"},
      {"com.hammerandchisel.discord", "Discord"},
      {"com.facebook.Messenger", "Messenger"},
      {"net.whatsapp.WhatsApp", "WhatsApp"},
      {"com.burbn.instagram", "Instagram"},
      {"com.linkedin.LinkedIn", "LinkedIn"},
      {"com.reddit.Reddit", "Reddit"},
      {"com.atebits.Tweetie2", "X"},
  };
  for (auto& k : known) {
    if (bundleId.equalsIgnoreCase(k[0])) return String(k[1]);
  }
  int dot = bundleId.lastIndexOf('.');
  String name = (dot >= 0) ? bundleId.substring(dot + 1) : bundleId;
  if (name.length() > 0) name.setCharAt(0, toupper(name[0]));
  if (name.length() > 14) name = name.substring(0, 14);
  return name;
}

static int textWidth(const String& s) {
  int16_t x1, y1;
  uint16_t w, h;
  g_canvas.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return x1 + w;
}

// Shortens a string with "..." so it fits in maxW pixels in the current font.
static String fitText(String s, int maxW) {
  s.replace('\n', ' ');
  if (textWidth(s) <= maxW) return s;
  while (s.length() > 1) {
    s.remove(s.length() - 1);
    if (textWidth(s + "...") <= maxW) break;
  }
  s.trim();
  return s + "...";
}

// Word-wraps text to maxW pixels in the current font.
static void wrapText(const String& text, int maxW, std::vector<String>& lines) {
  lines.clear();
  String line;
  String word;
  auto flushWord = [&]() {
    if (word.length() == 0) return;
    String candidate = line.length() ? line + " " + word : word;
    if (textWidth(candidate) <= maxW) {
      line = candidate;
    } else {
      if (line.length()) lines.push_back(line);
      // A single word wider than the screen is broken across lines.
      while (textWidth(word) > maxW && word.length() > 1) {
        int n = word.length() - 1;
        while (n > 1 && textWidth(word.substring(0, n)) > maxW) n--;
        lines.push_back(word.substring(0, n));
        word = word.substring(n);
      }
      line = word;
    }
    word = "";
  };
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == ' ') {
      flushWord();
    } else if (c == '\n') {
      flushWord();
      lines.push_back(line);
      line = "";
    } else {
      word += c;
    }
  }
  flushWord();
  if (line.length()) lines.push_back(line);
  // Drop blank lines at the end.
  while (!lines.empty() && lines.back().length() == 0) lines.pop_back();
}

// ---------------------------------------------------------------- storage

static int findNote(uint32_t uid) {
  for (size_t i = 0; i < g_notes.size(); i++) {
    if (g_notes[i].uid == uid) return (int)i;
  }
  return -1;
}

static int unreadCount() {
  int n = 0;
  for (auto& note : g_notes) {
    if (note.unread) n++;
  }
  return n;
}

static void clampSelection() {
  int count = (int)g_notes.size();
  if (g_sel >= count) g_sel = count - 1;
  if (g_sel < 0) g_sel = 0;
  if (g_sel < g_listTop) g_listTop = g_sel;
  if (g_sel >= g_listTop + rowsVisible()) g_listTop = g_sel - rowsVisible() + 1;
  if (g_listTop < 0) g_listTop = 0;
}

static void removeNote(uint32_t uid) {
  int idx = findNote(uid);
  if (idx < 0) return;
  g_notes.erase(g_notes.begin() + idx);
  if (g_screen == SCREEN_DETAIL && g_detailUid == uid) g_screen = g_detailReturn;
  if (idx < g_sel) g_sel--;
  clampSelection();
  markDirty(300);
}

static void openDetail(int idx) {
  if (idx < 0 || idx >= (int)g_notes.size()) return;
  if (g_screen != SCREEN_DETAIL) g_detailReturn = g_screen;
  g_sel = idx;
  clampSelection();
  g_notes[idx].unread = false;
  g_detailUid = g_notes[idx].uid;
  g_detailTop = 0;
  g_screen = SCREEN_DETAIL;
}

static void blinkLed(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_LED, HIGH);
    delay(60);
    digitalWrite(PIN_LED, LOW);
    delay(90);
  }
}

static uint32_t fingerprint(const Note& note) {
  uint32_t h = 2166136261u;
  auto mix = [&h](const String& text) {
    for (size_t i = 0; i < text.length(); i++) h = (h ^ (uint8_t)text[i]) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
  };
  mix(note.app);
  mix(note.title);
  mix(note.body);
  return h ? h : 1;
}

// Records a notification fingerprint; returns true if it was already known.
static bool seenRemember(uint32_t print) {
  for (int i = 0; i < g_rtc.seenCount; i++) {
    if (g_rtc.seen[i] == print) return true;
  }
  g_rtc.seen[g_rtc.seenNext] = print;
  g_rtc.seenNext = (g_rtc.seenNext + 1) % SEEN_SLOTS;
  if (g_rtc.seenCount < SEEN_SLOTS) g_rtc.seenCount++;
  return false;
}

static void wakeScreen();

static void storeNote(const PendingFetch& f, const String& appId, const String& title,
                      const String& subtitle, const String& message) {
  Note note;
  note.uid = f.uid;
  note.category = f.category;
  note.unread = !f.preexisting;
  note.app = appName(appId);
  note.title = title;
  note.title.replace('\n', ' ');
  note.title.trim();
  note.body = subtitle.length() ? subtitle + "\n" + message : message;
  note.body.trim();
  if (note.title.length() == 0) note.title = note.app;

  // After a sleep the phone re-sends everything it is still showing. Anything
  // not seen before the sleep is new, so mark it unread.
  uint32_t print = fingerprint(note);
  bool seenBefore = seenRemember(print);
  bool arrivedAsleep = f.preexisting && g_fromSleep && !seenBefore && findNote(f.uid) < 0;
  if (arrivedAsleep) {
    note.unread = true;
    g_newSinceSleep++;
  }
  if (f.announce) g_lastActivityMs = millis();

  int idx = findNote(f.uid);
  if (idx >= 0) {
    note.unread = g_notes[idx].unread;
    g_notes[idx] = note;
    markDirty(300);
    return;
  }

  g_notes.insert(g_notes.begin(), note);
  if (g_notes.size() > MAX_NOTES) g_notes.pop_back();
  if (g_screen == SCREEN_LIST && g_sel > 0) g_sel++;  // keep the same row selected
  clampSelection();

  if (f.announce && g_quiet) wakeScreen();
  if (f.announce && g_screen != SCREEN_SETTINGS) {
    openDetail(0);
    g_popupActive = true;
    g_popupAtMs = millis();
    g_popupUid = g_detailUid;
    blinkLed(3);
    markDirty(0);
  } else {
    markDirty(500);
  }
  Serial.printf("[ancs] %s | %s | %s\n", note.app.c_str(), note.title.c_str(), note.body.c_str());
}

// ------------------------------------------------------------- Bluetooth

static void onNotificationSource(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 8) return;
  NsEvent ev;
  ev.eventId = data[0];
  ev.flags = data[1];
  ev.category = data[2];
  ev.uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) |
           ((uint32_t)data[7] << 24);
  xQueueSend(g_nsQueue, &ev, 0);
  g_linkNotes = g_linkNotes + 1;
}

static void onDataSource(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  xStreamBufferSend(g_dsStream, data, len, 0);
}

static void pushInfo(uint8_t kind, uint8_t a, uint8_t b, const uint8_t* data, size_t len) {
  InfoEvent ev = {};
  ev.kind = kind;
  ev.a = a;
  ev.b = b;
  if (len > sizeof(ev.data)) len = sizeof(ev.data);
  ev.len = (uint8_t)len;
  if (data && len) memcpy(ev.data, data, len);
  xQueueSend(g_infoQueue, &ev, 0);
}

static void onCurrentTime(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  pushInfo(INFO_TIME, 0, 0, data, len);
}

static void onBatteryLevel(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len >= 1) pushInfo(INFO_BATTERY, data[0], 0, nullptr, 0);
}

// Each update is: entity, attribute, flags, then the value as text.
static void onMediaUpdate(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len >= 3) pushInfo(INFO_MEDIA, data[0], data[1], data + 3, len - 3);
}

// ------------------------------------------------------------- app link
// A private Bluetooth service for the companion iPhone app ("Inkfob"). The app
// can put weather and calendar lines on the home screen, change settings, send
// text files to read, and install new firmware. Only a paired phone can use it.
static const NimBLEUUID APP_SERVICE("1F0B0001-8A4E-4C1B-9E2D-5B7A1C3D9F10");
static const NimBLEUUID APP_CONTROL("1F0B0002-8A4E-4C1B-9E2D-5B7A1C3D9F10");  // commands in
static const NimBLEUUID APP_EVENT("1F0B0003-8A4E-4C1B-9E2D-5B7A1C3D9F10");    // replies out
static const NimBLEUUID APP_DATA("1F0B0004-8A4E-4C1B-9E2D-5B7A1C3D9F10");     // file / firmware bytes

struct AppCommand {
  char cmd;
  uint16_t len;
  char text[200];
};
static QueueHandle_t g_appMainQueue;   // commands the main loop handles (they touch the screen)
static QueueHandle_t g_appWorkQueue;   // commands the transfer task handles (storage, firmware)
static StreamBufferHandle_t g_appData;
static NimBLECharacteristic* g_appEvent = nullptr;
static volatile bool g_appListening = false;   // the app has subscribed to replies
static volatile uint32_t g_appListenAtMs = 0;
static volatile uint8_t g_xferKind = 0;        // 0 none, 1 file, 2 firmware
static volatile uint32_t g_xferSize = 0;
static volatile uint32_t g_xferGot = 0;
static volatile bool g_filesChanged = false;
static bool g_fsReady = false;
// Set when this firmware's Bluetooth layout is newer than the phone has seen,
// so the phone is told to look again and finds the app-link service.
static volatile bool g_layoutChanged = false;
static const uint8_t GATT_LAYOUT = 2;
extern "C" void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle);

static String g_infoWeather;                   // home screen lines from the app
static String g_infoEvents[2];
static String g_infoExtra;                     // steps today, from the app
static String g_infoMore[3];                   // countdown, reminder, ticker
static String g_quote;                         // the day's quote or fact
static String g_cardName = CARD_NAME;
static String g_cardLine1 = CARD_LINE1;
static String g_cardLine2 = CARD_LINE2;
static String g_cardQr = CARD_QR;
static bool g_cardQrStale = true;

static void appNotify(char kind, const uint8_t* data, size_t len) {
  if (!g_appEvent || !g_appListening || !g_connected) return;
  uint8_t buf[200];
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  buf[0] = (uint8_t)kind;
  if (len) memcpy(buf + 1, data, len);
  g_appEvent->setValue(buf, len + 1);
  g_appEvent->notify();
}

static void appNotifyText(char kind, const String& text) {
  appNotify(kind, (const uint8_t*)text.c_str(), text.length());
}

// Sends a long text as several replies, then an end marker.
static void appSendList(char kind, char endKind, const String& text, const String& tail) {
  for (size_t at = 0; at < text.length(); at += 160) {
    appNotifyText(kind, text.substring(at, at + 160));
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  appNotifyText(endKind, tail);
}

static String cleanFileName(const char* raw) {
  String name;
  for (const char* p = raw; *p && name.length() < 31; p++) {
    char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_' || c == '.';
    if (ok) name += c;
  }
  name.trim();
  return name;
}

class AppControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    NimBLEAttValue value = chr->getValue();
    if (value.size() < 1) return;
    AppCommand c = {};
    c.cmd = (char)value.data()[0];
    c.len = (uint16_t)min((size_t)(value.size() - 1), sizeof(c.text) - 1);
    memcpy(c.text, value.data() + 1, c.len);
    bool forMain = (c.cmd == 'I' || c.cmd == 'S' || c.cmd == 'J' || c.cmd == 'Q' || c.cmd == 'W' || c.cmd == 'M');
    xQueueSend(forMain ? g_appMainQueue : g_appWorkQueue, &c, 0);
  }
};

class AppDataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    NimBLEAttValue value = chr->getValue();
    // Waits when the buffer is full, which makes the phone slow down.
    if (g_xferKind) xStreamBufferSend(g_appData, value.data(), value.size(), pdMS_TO_TICKS(3000));
  }
};

class AppEventCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    g_appListening = (subValue != 0);
    g_appListenAtMs = millis();
  }
};

static String settingsText() {
  String s;
  s += "version=" + String(FIRMWARE_VERSION) + "\n";
  s += "small=" + String(g_smallText ? 1 : 0) + "\n";
  s += "flip=" + String(g_flip ? 1 : 0) + "\n";
  s += "fast=" + String(g_fastRefresh ? 1 : 0) + "\n";
  s += "sleep=" + String((int)g_sleepChoice) + "\n";
  s += "check=" + String((int)g_checkChoice) + "\n";
  s += "face=" + String((int)g_sleepFace) + "\n";
  s += "home=" + String((int)g_face) + "\n";
  s += "popup=" + String((int)g_popupChoice) + "\n";
  s += "btn2=" + String(g_twoButton ? 1 : 0) + "\n";
  s += "read=" + String((int)g_readTurn) + "\n";
  s += "exped=" + String(g_expedition ? 1 : 0) + "\n";
  s += "todo=" + String((int)g_todo.size()) + "\n";
  s += "corners=";
  for (int i = 0; i < 4; i++) s += String((int)g_corners[i]);
  s += "\n";
  s += "name=" + g_cardName + "\n";
  s += "line1=" + g_cardLine1 + "\n";
  s += "line2=" + g_cardLine2 + "\n";
  s += "qr=" + g_cardQr + "\n";
  return s;
}

// ---- Power saving -------------------------------------------------------
// Between jobs the chip naps (light sleep) and Bluetooth only wakes for its
// own radio slots. Drawing the screen and file transfers hold it awake.
static bool g_saverOn = false;                 // napping is switched on for this boot
static esp_pm_lock_handle_t g_awakeLock = nullptr;
static bool g_awakeHeld = false;
static volatile bool g_inputBusy = false;      // a button is down or a tap is being timed
static bool g_linkSlow = false;                // the phone was asked for the relaxed link
static uint32_t g_linkAskedGen = 0;
static uint32_t g_linkAskAtMs = 0;
static bool g_bootCounted = false;

static void startPowerSaving(bool skip);
static void setExpedition(bool on);
static void openReader(int item, int page);
static void setFix(const String& text);
static bool addWaypoint(const String& wanted);
static void servicePower();
static void idleWait(uint32_t ms);

static void stayAwake(bool on) {
  if (!g_awakeLock || on == g_awakeHeld) return;
  if (on) esp_pm_lock_acquire(g_awakeLock); else esp_pm_lock_release(g_awakeLock);
  g_awakeHeld = on;
}

// Storage, file lists and firmware installs: slow work kept off the main loop.
static void transferTask(void*) {
  File file;
  String filePath;
  uint32_t lastDataMs = 0;
  uint32_t lastReport = 0;
  static uint8_t buf[1024];
  auto fail = [&](const char* why) {
    if (g_xferKind == 1 && file) {
      file.close();
      LittleFS.remove(filePath);
    }
    if (g_xferKind == 2) Update.abort();
    g_xferKind = 0;
    xStreamBufferReset(g_appData);
    appNotifyText('X', why);
    Serial.printf("[app] transfer failed: %s\n", why);
  };
  for (;;) {
    AppCommand c;
    if (xQueueReceive(g_appWorkQueue, &c, pdMS_TO_TICKS(g_xferKind ? 2 : 50)) == pdTRUE) {
      if (c.cmd == 'G') {
        appSendList('g', 'h', settingsText(), "");
      } else if (c.cmd == 'L') {
        String list;
        if (g_fsReady) {
          File root = LittleFS.open("/");
          for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            list += String(f.name()) + "\t" + String((uint32_t)f.size()) + "\n";
          }
        }
        uint32_t freeBytes = g_fsReady ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
        appSendList('l', 'm', list, String(freeBytes));
      } else if (c.cmd == 'D') {
        String name = cleanFileName(c.text);
        bool ok = g_fsReady && name.length() && LittleFS.remove("/" + name);
        g_filesChanged = true;
        appNotifyText(ok ? 'K' : 'X', ok ? "deleted" : "no such file");
      } else if (c.cmd == 'C') {
        if (g_xferKind) fail("cancelled");
      } else if ((c.cmd == 'N' || c.cmd == 'F') && c.len >= 5) {
        if (g_xferKind) fail("replaced by a new transfer");
        uint32_t size;
        memcpy(&size, c.text, 4);
        xStreamBufferReset(g_appData);
        g_xferGot = 0;
        lastReport = 0;
        lastDataMs = millis();
        if (c.cmd == 'N') {
          String name = cleanFileName(c.text + 4);
          uint32_t room = g_fsReady ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
          if (!g_fsReady || !name.length()) {
            appNotifyText('X', "storage not ready or bad name");
          } else if (size + 8192 > room) {
            appNotifyText('X', "not enough space");
          } else {
            filePath = "/" + name;
            file = LittleFS.open(filePath, "w");
            if (!file) {
              appNotifyText('X', "could not create the file");
            } else {
              g_xferSize = size;
              g_xferKind = 1;
              appNotifyText('A', "");
            }
          }
        } else {
          char md5[33] = {};
          if (c.len >= 36) memcpy(md5, c.text + 4, 32);
          if (!Update.begin(size)) {
            appNotifyText('X', "no room for the update");
          } else {
            if (md5[0]) Update.setMD5(md5);
            g_xferSize = size;
            g_xferKind = 2;
            appNotifyText('A', "");
            Serial.printf("[app] firmware update started, %u bytes\n", (unsigned)size);
          }
        }
      }
    }

    if (!g_xferKind) continue;
    size_t n = xStreamBufferReceive(g_appData, buf, sizeof(buf), 0);
    if (n > 0) {
      lastDataMs = millis();
      uint32_t left = g_xferSize - g_xferGot;
      if (n > left) n = left;
      size_t wrote = (g_xferKind == 1) ? file.write(buf, n) : Update.write(buf, n);
      if (wrote != n) {
        fail("write failed");
        continue;
      }
      g_xferGot = g_xferGot + n;
      if (g_xferGot - lastReport >= 16384 || g_xferGot == g_xferSize) {
        lastReport = g_xferGot;
        uint32_t got = g_xferGot;
        appNotify('P', (const uint8_t*)&got, 4);
      }
      if (g_xferGot == g_xferSize) {
        if (g_xferKind == 1) {
          file.close();
          g_xferKind = 0;
          g_filesChanged = true;
          appNotifyText('K', "saved");
        } else if (Update.end(true)) {
          appNotifyText('K', "installed");
          Serial.println("[app] firmware installed, restarting");
          vTaskDelay(pdMS_TO_TICKS(1200));
          ESP.restart();
        } else {
          g_xferKind = 0;
          appNotifyText('X', "the update did not verify");
        }
      }
    } else if (!g_connected || millis() - lastDataMs > 20000) {
      fail(g_connected ? "timed out" : "phone disconnected");
    }
  }
}

static void startAppLink(NimBLEServer* server) {
  g_appMainQueue = xQueueCreate(6, sizeof(AppCommand));
  g_appWorkQueue = xQueueCreate(6, sizeof(AppCommand));
  g_appData = xStreamBufferCreate(16384, 1);
  NimBLEService* svc = server->createService(APP_SERVICE);
  static AppControlCallbacks controlCallbacks;
  static AppDataCallbacks dataCallbacks;
  static AppEventCallbacks eventCallbacks;
  svc->createCharacteristic(APP_CONTROL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 512)
      ->setCallbacks(&controlCallbacks);
  svc->createCharacteristic(APP_DATA, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 512)
      ->setCallbacks(&dataCallbacks);
  g_appEvent = svc->createCharacteristic(APP_EVENT, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC, 200);
  g_appEvent->setCallbacks(&eventCallbacks);
  xTaskCreatePinnedToCore(transferTask, "pager-xfer", 8192, nullptr, 2, nullptr, 1);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    Serial.printf("[ble] connected: %s\n", info.getAddress().toString().c_str());
    g_connHandle = info.getConnHandle();
    g_connGeneration = g_connGeneration + 1;
    g_connected = true;
    g_secured = false;
    g_connectCount = g_connectCount + 1;
    g_linkStartMs = millis();
    g_linkStage = 0;
    g_linkNotes = 0;
    // A phone that is not paired yet is asked to pair straight away. A phone
    // that is already paired re-encrypts by itself; asking at the same moment
    // can trip it up, so the Bluetooth task only nudges it if it has not.
    if (NimBLEDevice::getNumBonds() == 0) NimBLEDevice::startSecurity(info.getConnHandle());
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("[ble] disconnected, reason %d\n", reason);
    g_connected = false;
    g_secured = false;
    g_appListening = false;
    g_dropReason = reason;
    g_dropAtMs = millis();
    g_dropCount = g_dropCount + 1;
    LinkRecord& rec = g_linkLog[g_linkLogCount % LINK_LOG_SIZE];
    rec.endMs = millis();
    rec.seconds = (millis() - g_linkStartMs) / 1000;
    rec.stage = g_linkStage;
    rec.reason = (uint8_t)(reason & 0xFF);
    rec.notes = g_linkNotes;
    g_linkLogCount = g_linkLogCount + 1;
    g_fastAdvUntilMs = millis() + 30000;  // be easy to find again
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[ble] security: encrypted=%d bonded=%d\n", info.isEncrypted(), info.isBonded());
    if (info.isEncrypted()) {
      g_secured = true;
      if (g_linkStage < 1) g_linkStage = 1;
    }
  }
};

static ServerCallbacks g_serverCallbacks;

// A tiny "media keys" HID service. It is here so the pager shows up under
// Settings > Bluetooth on the iPhone and so iOS reconnects to it by itself.
// It never sends any keys, and it does not hide the on-screen keyboard.
static uint8_t g_hidReportMap[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x09, 0xB5,        //   Usage (Scan Next Track)
    0x09, 0xB6,        //   Usage (Scan Previous Track)
    0x09, 0xCD,        //   Usage (Play/Pause)
    0x09, 0xE9,        //   Usage (Volume Up)
    0x09, 0xEA,        //   Usage (Volume Down)
    0x09, 0xE2,        //   Usage (Mute)
    0x0A, 0x23, 0x02,  //   Usage (AC Home)
    0x0A, 0x24, 0x02,  //   Usage (AC Back)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection
};

static void startBluetooth() {
  NimBLEDevice::init("Pager");
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no passkey, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(9);  // stronger signal: the battery and case sit over the antenna

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&g_serverCallbacks);
  server->advertiseOnDisconnect(true);

  static NimBLEHIDDevice hid(server);
  hid.setManufacturer("DIY");
  hid.setPnp(0x02, 0x303A, 0x8213, 0x0100);
  hid.setHidInfo(0x00, 0x01);
  hid.setReportMap(g_hidReportMap, sizeof(g_hidReportMap));
  g_hidInput = hid.getInputReport(1);
  hid.setBatteryLevel(100);
  startAppLink(server);
  server->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // The request for the phone's notification service must be in the main
  // advertising packet: a phone scanning in the background never asks for the
  // scan response, so it would not see it there and would not reconnect.
  static const uint8_t solicit[] = {0x11, 0x15, 0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F,
                                    0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79};
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.addData(solicit, sizeof(solicit));
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  adv->setAdvertisementData(advData);

  // Scan response: the name and icon type, shown when the phone looks closer.
  NimBLEAdvertisementData scanData;
  scanData.setName("Pager");
  scanData.setAppearance(0x03C0);  // generic HID
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setAdvertisingInterval(32);
  g_advFast = true;
  g_fastAdvUntilMs = millis() + 30000;
  if (!g_expedition) adv->start();
  Serial.println("[ble] advertising as \"Pager\"");
}

// Finds the phone's notification service and subscribes to it. Returns the
// control point, or nullptr if the phone is not offering the service yet.
// Runs in the Bluetooth task only.
static NimBLERemoteCharacteristic* discoverAncs(NimBLEClient** clientOut) {
  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server || !g_connected) return nullptr;
  // Note: every getClient() call throws away what was discovered before, so
  // it is called once here and the same client is reused for the extras.
  NimBLEClient* client = server->getClient(g_connHandle);
  if (!client) return nullptr;

  NimBLERemoteService* service = client->getService(ANCS_SERVICE);
  if (!service) {
    Serial.println("[ancs] service not found yet");
    return nullptr;
  }
  NimBLERemoteCharacteristic* ns = service->getCharacteristic(ANCS_NOTIFICATION_SOURCE);
  NimBLERemoteCharacteristic* cp = service->getCharacteristic(ANCS_CONTROL_POINT);
  NimBLERemoteCharacteristic* ds = service->getCharacteristic(ANCS_DATA_SOURCE);
  if (!ns || !cp || !ds) {
    Serial.printf("[ancs] characteristics missing: source=%d control=%d data=%d\n", ns != nullptr,
                  cp != nullptr, ds != nullptr);
    return nullptr;
  }
  // Data Source first, so nothing is missed once notifications start arriving.
  if (!ds->subscribe(true, onDataSource)) return nullptr;
  if (!ns->subscribe(true, onNotificationSource)) return nullptr;
  Serial.println("[ancs] subscribed");
  *clientOut = client;
  return cp;
}

// Clock, phone battery and now-playing. All optional: anything missing is
// skipped. Returns the media remote characteristic, or nullptr.
static NimBLERemoteCharacteristic* discoverExtras(NimBLEClient* client) {
  if (NimBLERemoteService* svc = client->getService(TIME_SERVICE)) {
    if (NimBLERemoteCharacteristic* chr = svc->getCharacteristic(TIME_CURRENT)) {
      NimBLEAttValue value = chr->readValue();
      if (value.size() >= 7) pushInfo(INFO_TIME, 0, 0, value.data(), value.size());
      if (chr->canNotify()) chr->subscribe(true, onCurrentTime);
      Serial.printf("[time] read %d bytes\n", (int)value.size());
    }
  } else {
    Serial.println("[time] service not offered");
  }

  if (NimBLERemoteService* svc = client->getService(BATTERY_SERVICE)) {
    if (NimBLERemoteCharacteristic* chr = svc->getCharacteristic(BATTERY_LEVEL)) {
      NimBLEAttValue value = chr->readValue();
      if (value.size() >= 1) pushInfo(INFO_BATTERY, value.data()[0], 0, nullptr, 0);
      if (chr->canNotify()) chr->subscribe(true, onBatteryLevel);
      Serial.printf("[battery] read %d bytes\n", (int)value.size());
    }
  } else {
    Serial.println("[battery] service not offered");
  }

  NimBLERemoteCharacteristic* remote = nullptr;
  if (NimBLERemoteService* svc = client->getService(AMS_SERVICE)) {
    remote = svc->getCharacteristic(AMS_REMOTE_COMMAND);
    NimBLERemoteCharacteristic* update = svc->getCharacteristic(AMS_ENTITY_UPDATE);
    if (update && update->subscribe(true, onMediaUpdate)) {
      const uint8_t wantTrack[] = {0x02, 0x00, 0x02};   // track: artist, title
      const uint8_t wantPlayer[] = {0x00, 0x00, 0x01};  // player: app name, playback state
      update->writeValue(wantTrack, sizeof(wantTrack), true);
      update->writeValue(wantPlayer, sizeof(wantPlayer), true);
      Serial.println("[media] subscribed");
    } else {
      Serial.println("[media] could not subscribe");
    }
  } else {
    Serial.println("[media] service not offered");
  }
  return remote;
}

// Everything that can block on the radio happens here, so the buttons and the
// screen never wait for the phone.
static void bluetoothTask(void*) {
  uint32_t seenGeneration = 0;
  uint32_t nextDiscoverMs = 0;
  uint32_t nextAdvertiseCheckMs = 0;
  uint32_t nextSecurityMs = 0;
  NimBLERemoteCharacteristic* controlPoint = nullptr;
  NimBLERemoteCharacteristic* mediaRemote = nullptr;

  for (;;) {
    uint32_t generation = g_connGeneration;
    if (generation != seenGeneration) {
      seenGeneration = generation;
      controlPoint = nullptr;
      mediaRemote = nullptr;
      g_ancsReady = false;
      g_discoverTries = 0;
      nextDiscoverMs = millis() + 300;
      nextSecurityMs = millis() + 3000;
      xQueueReset(g_requestQueue);
    }

    if (g_sleeping) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!g_connected) {
      controlPoint = nullptr;
      mediaRemote = nullptr;
      g_ancsReady = false;
      // Make sure the phone can always find the pager again.
      if ((int32_t)(millis() - nextAdvertiseCheckMs) >= 0) {
        nextAdvertiseCheckMs = millis() + 1000;
        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        // Apple's guidance: advertise every 20 ms for the first 30 seconds,
        // then every 152.5 ms, so the phone spots the pager quickly.
        bool wantFast = (int32_t)(millis() - g_fastAdvUntilMs) < 0;
        if (adv && wantFast != g_advFast) {
          adv->stop();
          adv->setAdvertisingInterval(wantFast ? 32 : 244);
          g_advFast = wantFast;
        }
        if (adv && !adv->isAdvertising() && !g_expedition) adv->start();
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!g_secured) {
      // Pairing has not finished; nudge the phone again every few seconds.
      if ((int32_t)(millis() - nextSecurityMs) >= 0) {
        nextSecurityMs = millis() + 5000;
        NimBLEDevice::startSecurity(g_connHandle);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (!g_ancsReady) {
      // Wait until the main loop has cleared out the previous connection.
      if (g_loopGeneration == generation && (int32_t)(millis() - nextDiscoverMs) >= 0) {
        NimBLEClient* client = nullptr;
        controlPoint = discoverAncs(&client);
        if (controlPoint) {
          g_ancsReady = true;  // notifications start flowing straight away
          g_linkStage = 2;
          if (g_layoutChanged) {
            g_layoutChanged = false;
            ble_svc_gatt_changed(0x0001, 0xffff);
            g_prefs.putUChar("gattv", GATT_LAYOUT);
            Serial.println("[app] told the phone the Bluetooth layout changed");
          }
          mediaRemote = discoverExtras(client);
        } else {
          uint8_t tries = g_discoverTries;
          if (tries < 250) g_discoverTries = tries + 1;
          nextDiscoverMs = millis() + (tries < 5 ? 2000 : 10000);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    BleRequest req;
    if (xQueueReceive(g_requestQueue, &req, pdMS_TO_TICKS(50)) == pdTRUE) {
      NimBLERemoteCharacteristic* target = (req.target == 1) ? mediaRemote : controlPoint;
      bool ok = g_connected && target && target->writeValue(req.data, req.len, true);
      // The phone rejects a request for a notification that has already gone.
      if (!ok && req.isFetch) {
        g_failedUid = req.uid;
        g_fetchFailed = true;
      }
    }
  }
}

static void requestAttributes(uint32_t uid) {
  BleRequest req = {};
  const uint8_t bytes[] = {
      0x00,  // Get Notification Attributes
      (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
      0x00,                                                       // app identifier
      0x01, (uint8_t)MAX_TITLE, (uint8_t)(MAX_TITLE >> 8),        // title
      0x02, (uint8_t)MAX_SUBTITLE, (uint8_t)(MAX_SUBTITLE >> 8),  // subtitle
      0x03, (uint8_t)MAX_MESSAGE, (uint8_t)(MAX_MESSAGE >> 8),    // message
  };
  memcpy(req.data, bytes, sizeof(bytes));
  req.len = sizeof(bytes);
  req.isFetch = true;
  req.uid = uid;
  if (xQueueSend(g_requestQueue, &req, 0) != pdTRUE) {
    g_failedUid = uid;
    g_fetchFailed = true;
  }
}

// Media remote commands: 2 play/pause, 3 next, 4 previous, 5 volume up, 6 volume down.
static void sendMediaCommand(uint8_t command) {
  BleRequest req = {};
  req.data[0] = command;
  req.len = 1;
  req.target = 1;
  xQueueSend(g_requestQueue, &req, 0);
}

static void performAction(uint32_t uid, uint8_t action) {
  BleRequest req = {};
  const uint8_t bytes[] = {0x02, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16),
                           (uint8_t)(uid >> 24), action};
  memcpy(req.data, bytes, sizeof(bytes));
  req.len = sizeof(bytes);
  req.isFetch = false;
  req.uid = uid;
  xQueueSend(g_requestQueue, &req, 0);
}

// Tries to parse a complete reply for the notification being fetched.
// Returns true once the reply has been fully handled (or thrown away).
static bool parseReply() {
  if (g_rxLen < 5) return false;
  uint32_t uid = (uint32_t)g_rx[1] | ((uint32_t)g_rx[2] << 8) | ((uint32_t)g_rx[3] << 16) |
                 ((uint32_t)g_rx[4] << 24);
  if (g_rx[0] != 0x00 || uid != g_fetch.uid) return true;  // not ours: drop it

  String fields[4];
  size_t pos = 5;
  for (int i = 0; i < 4; i++) {
    if (pos + 3 > g_rxLen) return false;
    uint8_t id = g_rx[pos];
    uint16_t len = (uint16_t)g_rx[pos + 1] | ((uint16_t)g_rx[pos + 2] << 8);
    pos += 3;
    if (len > sizeof(g_rx)) return true;  // nonsense length: drop it
    if (pos + len > g_rxLen) return false;
    if (id < 4) fields[id] = toAscii(g_rx + pos, len);
    pos += len;
  }
  storeNote(g_fetch, fields[0], fields[1], fields[2], fields[3]);
  return true;
}

// The main loop's half of the Bluetooth work: it never blocks.
static void serviceBluetooth() {
  static uint32_t seenGeneration = 0;
  static bool wasConnected = false;
  static bool wasSecured = false;
  static bool wasReady = false;
  static uint8_t wasTries = 0;

  uint32_t generation = g_connGeneration;
  if (generation != seenGeneration) {
    // Anything learned about the previous connection is no longer valid.
    seenGeneration = generation;
    g_pending.clear();
    g_fetching = false;
    g_rxLen = 0;
    xQueueReset(g_nsQueue);
    xStreamBufferReset(g_dsStream);
    g_loopGeneration = generation;  // lets the Bluetooth task start talking to the phone
  }

  bool connected = g_connected;
  bool secured = g_secured;
  bool ready = g_ancsReady;
  uint8_t tries = g_discoverTries;
  if (connected != wasConnected || secured != wasSecured || ready != wasReady ||
      (g_screen == SCREEN_STATUS && tries != wasTries)) {
    wasConnected = connected;
    wasSecured = secured;
    if (ready && !wasReady) g_readyAtMs = millis();
    wasReady = ready;
    wasTries = tries;
    if (!connected) {
      g_pending.clear();
      g_fetching = false;
    }
    markDirty(300);
  }
  if (!ready) return;

  // New / changed / removed notifications.
  NsEvent ev;
  while (xQueueReceive(g_nsQueue, &ev, 0) == pdTRUE) {
    g_lastNsMs = millis();
    if (ev.eventId == 2) {
      removeNote(ev.uid);
      for (size_t i = 0; i < g_pending.size(); i++) {
        if (g_pending[i].uid == ev.uid) {
          g_pending.erase(g_pending.begin() + i);
          break;
        }
      }
      continue;
    }
    bool silent = ev.flags & 0x01;
    bool preexisting = ev.flags & 0x04;
    PendingFetch f;
    f.uid = ev.uid;
    f.category = ev.category;
    f.announce = (ev.eventId == 0) && !silent && !preexisting;
    f.preexisting = preexisting;
    bool queued = false;
    for (auto& p : g_pending) {
      if (p.uid == f.uid) queued = true;
    }
    if (!queued && g_pending.size() < 100) g_pending.push_back(f);
  }

  // Reply bytes for the notification being fetched.
  if (g_fetching) {
    size_t room = sizeof(g_rx) - g_rxLen;
    if (room > 0) g_rxLen += xStreamBufferReceive(g_dsStream, g_rx + g_rxLen, room, 0);
    bool failed = g_fetchFailed && g_failedUid == g_fetch.uid;
    if (failed || parseReply() || millis() - g_fetchStartMs > 4000) {
      g_fetching = false;
      g_rxLen = 0;
    }
  }

  if (!g_fetching && !g_pending.empty()) {
    g_fetch = g_pending.front();
    g_pending.erase(g_pending.begin());
    g_fetching = true;
    g_fetchFailed = false;
    g_fetchStartMs = millis();
    g_rxLen = 0;
    xStreamBufferReset(g_dsStream);
    requestAttributes(g_fetch.uid);
  }
}

// ------------------------------------------------------------------ clock

// Days since 1970-01-01 for a calendar date, and back again.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = (int)(z - era * 146097);
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

// The phone sends: year (2 bytes), month, day, hour, minute, second, ...
static void setTimeFromPhone(const uint8_t* d, size_t len) {
  if (len < 7) return;
  int year = d[0] | (d[1] << 8);
  if (year < 2020 || d[2] < 1 || d[2] > 12 || d[3] < 1 || d[3] > 31) return;
  g_timeBase = daysFromCivil(year, d[2], d[3]) * 86400LL + d[4] * 3600 + d[5] * 60 + d[6];
  g_timeBaseMs = millis();
  g_timeKnown = true;
}

struct ClockNow {
  int hour, minute, weekday, month, day;  // weekday: 0 = Sunday
};

static bool clockNow(ClockNow& now) {
  if (!g_timeKnown) return false;
  int64_t t = g_timeBase + (uint32_t)(millis() - g_timeBaseMs) / 1000;
  int64_t days = t / 86400;
  int secs = (int)(t % 86400);
  int year;
  civilFromDays(days, year, now.month, now.day);
  now.hour = secs / 3600;
  now.minute = (secs % 3600) / 60;
  now.weekday = (int)((days + 4) % 7);  // 1970-01-01 was a Thursday
  return true;
}

// Takes in clock, battery and now-playing updates from the phone.
static void serviceInfo() {
  InfoEvent ev;
  bool changed = false;
  while (xQueueReceive(g_infoQueue, &ev, 0) == pdTRUE) {
    if (ev.kind == INFO_TIME) {
      setTimeFromPhone(ev.data, ev.len);
      changed = true;
    } else if (ev.kind == INFO_BATTERY) {
      if (g_phoneBattery != ev.a) changed = true;
      g_phoneBattery = ev.a;
    } else if (ev.kind == INFO_MEDIA) {
      String value = toAscii(ev.data, ev.len);
      value.replace('\n', ' ');
      if (ev.a == 2 && ev.b == 0) g_mediaArtist = value;
      if (ev.a == 2 && ev.b == 2) g_mediaTitle = value;
      if (ev.a == 0 && ev.b == 0) g_mediaPlayer = value;
      if (ev.a == 0 && ev.b == 1) g_mediaPlaying = value.startsWith("1");  // "state,rate,elapsed"
      changed = true;
    }
  }
  if (!g_connected && g_phoneBattery >= 0) {
    g_phoneBattery = -1;
    changed = true;
  }
  if (changed && (g_screen == SCREEN_HOME || g_screen == SCREEN_MEDIA)) markDirty(600);

  // Redraw the clock when the minute changes.
  ClockNow now;
  if (g_screen == SCREEN_HOME && clockNow(now) && now.hour * 60 + now.minute != g_homeMinute) {
    markDirty(1);
  }
}

// ---------------------------------------------------------------- drawing

static void drawHeader(const String& left, const String& right) {
  g_canvas.fillRect(0, 0, EPD_WIDTH, HEADER_H, 1);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(0);
  g_canvas.setCursor(4, 3);
  g_canvas.print(left);
  g_canvas.setCursor(EPD_WIDTH - 4 - (int)right.length() * 6, 3);
  g_canvas.print(right);
}

static String linkStatus() {
  if (g_ancsReady) return "iPhone";
  if (g_secured) return "linking";
  if (g_connected) return "pairing";
  return "no phone";
}

static void drawCentered(const String& text, int baseline) {
  g_canvas.setCursor((EPD_WIDTH - textWidth(text)) / 2, baseline);
  g_canvas.print(text);
}

static void drawList() {
  int unread = unreadCount();
  String left = "PAGER";
  if (unread > 0) left += "  " + String(unread) + " new";
  drawHeader(left, linkStatus());

  g_canvas.setTextColor(1);
  if (g_notes.empty()) {
    g_canvas.setFont(&FreeSansBold9pt7b);
    if (g_ancsReady) {
      drawCentered("No notifications", 60);
    } else if (g_secured) {
      drawCentered("Paired", 50);
      g_canvas.setFont(&FreeSans9pt7b);
      drawCentered("Asking for notifications...", 78);
    } else if (g_connected) {
      drawCentered("Pairing...", 50);
      g_canvas.setFont(&FreeSans9pt7b);
      drawCentered("Tap Pair on the iPhone", 78);
    } else {
      drawCentered("Waiting for iPhone", 44);
      g_canvas.setFont(&FreeSans9pt7b);
      drawCentered("Settings > Bluetooth", 72);
      drawCentered("then tap \"Pager\"", 94);
    }
    return;
  }

  clampSelection();
  const int rowH = rowHeight();
  for (int row = 0; row < rowsVisible(); row++) {
    int idx = g_listTop + row;
    if (idx >= (int)g_notes.size()) break;
    const Note& note = g_notes[idx];
    int top = HEADER_H + row * rowH;
    bool selected = (idx == g_sel);
    if (selected) g_canvas.fillRect(0, top, EPD_WIDTH, rowH, 1);
    g_canvas.setTextColor(selected ? 0 : 1);

    const int textX = 12;
    const int maxW = EPD_WIDTH - textX - 5;
    if (note.unread) {
      g_canvas.fillCircle(5, top + (g_smallText ? 7 : 11), g_smallText ? 2 : 3, selected ? 0 : 1);
    }

    String heading = note.app;
    if (note.title.length() && note.title != note.app) heading += ": " + note.title;
    useFont(true);
    printLine(textX, top + (g_smallText ? 3 : 2), fitText(heading, maxW), true);
    useFont(false);
    printLine(textX, top + (g_smallText ? 15 : 18), fitText(note.body, maxW), false);

    if (!selected && row < rowsVisible() - 1) {
      g_canvas.drawFastHLine(0, top + rowH - 1, EPD_WIDTH, 1);
    }
  }
}

// Shown on the home screen until the phone is linked for the first time.
static void drawLinkHelp() {
  g_canvas.setTextColor(1);
  g_canvas.setFont(&FreeSansBold9pt7b);
  if (g_secured) {
    drawCentered("Paired", 50);
    g_canvas.setFont(&FreeSans9pt7b);
    drawCentered("Asking for notifications...", 78);
  } else if (g_connected) {
    drawCentered("Pairing...", 50);
    g_canvas.setFont(&FreeSans9pt7b);
    drawCentered("Tap Pair on the iPhone", 78);
  } else {
    drawCentered("Waiting for iPhone", 44);
    g_canvas.setFont(&FreeSans9pt7b);
    drawCentered("Settings > Bluetooth", 72);
    drawCentered("then tap \"Pager\"", 94);
  }
}

// One small centred line. The screen holds 40 of these characters, so a longer
// line first loses its wide gaps and, if that is not enough, wraps onto a second line.
static void drawSmallCentered(const String& original, int top) {
  const int MAX_CHARS = 40;
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  String text = original;
  while ((int)text.length() > MAX_CHARS && text.indexOf("   ") >= 0) text.replace("   ", "  ");
  if ((int)text.length() <= MAX_CHARS) {
    g_canvas.setCursor((EPD_WIDTH - (int)text.length() * 6) / 2, top);
    g_canvas.print(text);
    return;
  }
  // break at the space nearest the middle, preferring a wide gap between two hints
  int mid = text.length() / 2, cut = -1, best = 1000;
  for (int i = 1; i < (int)text.length() - 1; i++) {
    if (text[i] != ' ') continue;
    bool wide = text[i + 1] == ' ' || text[i - 1] == ' ';
    int score = abs(i - mid) - (wide ? 8 : 0);
    if (score < best) {
      best = score;
      cut = i;
    }
  }
  String first = cut > 0 ? text.substring(0, cut) : text.substring(0, MAX_CHARS);
  String second = cut > 0 ? text.substring(cut) : text.substring(MAX_CHARS);
  first.trim();
  second.trim();
  if ((int)first.length() > MAX_CHARS) first = first.substring(0, MAX_CHARS);
  if ((int)second.length() > MAX_CHARS) second = second.substring(0, MAX_CHARS);
  g_canvas.setCursor((EPD_WIDTH - (int)first.length() * 6) / 2, top);
  g_canvas.print(first);
  g_canvas.setCursor((EPD_WIDTH - (int)second.length() * 6) / 2, top + 10);
  g_canvas.print(second);
}

static bool photoReady();
static int unreadCount();

static String unreadText() {
  int unread = unreadCount();
  if (unread > 0) return String(unread) + " new";
  return "";
}

// The alternative home screens. Each one is drawn from the same few facts:
// the time, the date, the app's weather and calendar lines, and the unread count.
static void drawFace(const String& clock, const String& ampm, const String& date,
                     const String& shortDate) {
  String unread = unreadText();
  String link = g_ancsReady ? String("") : linkStatus();
  g_canvas.setTextColor(1);

  if (g_face == 1 || g_face == 4) {
    // Big clock, and the same with nothing else on it.
    bool minimal = (g_face == 4);
    g_canvas.setFont(&FreeSansBold24pt7b);
    g_canvas.setTextSize(2);
    int w = textWidth(clock);
    if (w > EPD_WIDTH - 4) {
      g_canvas.setTextSize(1);
      w = textWidth(clock);
    }
    g_canvas.setCursor((EPD_WIDTH - w) / 2 - 2, minimal ? 92 : 76);
    g_canvas.print(clock);
    g_canvas.setTextSize(1);
    if (minimal) return;
    g_canvas.drawFastHLine(8, 90, EPD_WIDTH - 16, 1);
    g_canvas.setFont(&FreeSansBold9pt7b);
    String left = shortDate + (ampm.length() ? "  " + ampm : "");
    g_canvas.setCursor(8, 110);
    g_canvas.print(left);
    String right = unread.length() ? unread : (link.length() ? link : g_infoExtra);
    right = fitText(right, EPD_WIDTH - textWidth(left) - 28);
    g_canvas.setCursor(EPD_WIDTH - 8 - textWidth(right), 110);
    g_canvas.print(right);
    return;
  }

  if (g_face == 2) {
    // Weather first: temperature and sky in large type, the rest underneath.
    String headline = g_infoWeather, detail;
    int cut = g_infoWeather.indexOf("  ");
    if (cut > 0) {
      headline = g_infoWeather.substring(0, cut);
      detail = g_infoWeather.substring(cut + 2);
    }
    if (!headline.length()) headline = "No weather yet";
    g_canvas.setFont(&FreeSansBold24pt7b);
    if (textWidth(headline) > EPD_WIDTH - 8) g_canvas.setFont(&FreeSansBold9pt7b);
    drawCentered(headline, 44);
    g_canvas.setFont(&FreeSansBold9pt7b);
    if (detail.length()) drawCentered(fitText(detail, EPD_WIDTH - 10), 68);
    g_canvas.drawFastHLine(8, 78, EPD_WIDTH - 16, 1);
    String when = clock + (ampm.length() ? " " + ampm : "") + "   " + shortDate;
    drawCentered(when, 98);
    String last = unread.length() ? unread + " notifications" : g_infoEvents[0];
    if (!last.length()) last = g_infoExtra;
    drawSmallCentered(fitText(last, EPD_WIDTH - 10), 108);
    return;
  }

  if (g_face == 3) {
    // Agenda: what is coming up, with the clock tucked into the top bar.
    drawHeader(clock + (ampm.length() ? " " + ampm : "") + "  " + shortDate,
               unread.length() ? unread : link);
    g_canvas.setTextColor(1);
    String rows[4];
    int count = 0;
    if (g_infoEvents[0].length()) rows[count++] = g_infoEvents[0];
    if (g_infoEvents[1].length()) rows[count++] = g_infoEvents[1];
    if (!count) rows[count++] = "Nothing coming up";
    if (g_infoWeather.length()) rows[count++] = g_infoWeather;
    if (g_infoExtra.length() && count < 4) rows[count++] = g_infoExtra;
    int y = 36;
    for (int i = 0; i < count; i++) {
      bool event = (i < 2 && rows[i] != g_infoWeather && rows[i] != g_infoExtra);
      g_canvas.setFont(event ? &FreeSansBold9pt7b : &FreeSans9pt7b);
      g_canvas.setCursor(6, y);
      g_canvas.print(fitText(rows[i], EPD_WIDTH - 12));
      y += 24;
    }
    return;
  }

  if (g_face == 6) {
    // Custom: the clock in the middle and a chosen fact in each corner.
    g_canvas.setFont(&FreeSansBold24pt7b);
    int clockW = textWidth(clock);
    g_canvas.setFont(&FreeSansBold9pt7b);
    int ampmW = ampm.length() ? textWidth(ampm) + 6 : 0;
    int x = (EPD_WIDTH - clockW - ampmW) / 2;
    g_canvas.setFont(&FreeSansBold24pt7b);
    g_canvas.setCursor(x, 76);
    g_canvas.print(clock);
    g_canvas.setFont(&FreeSansBold9pt7b);
    g_canvas.setCursor(x + clockW + 6, 76);
    g_canvas.print(ampm);
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    for (int i = 0; i < 4; i++) {
      String text;
      switch (g_corners[i]) {
        case 1: text = shortDate; break;
        case 2: {
          int cut = g_infoWeather.indexOf("  ");
          text = cut > 0 ? g_infoWeather.substring(0, cut) : g_infoWeather;
          break;
        }
        case 3: text = g_infoEvents[0]; break;
        case 4: text = g_infoExtra; text.replace(" today", ""); break;
        case 5: text = g_phoneBattery >= 0 ? "Phone " + String(g_phoneBattery) + "%" : link; break;
        case 6: text = unread.length() ? unread : String("0 new"); break;
        case 7: text = g_infoMore[0]; break;
        case 8: text = g_infoMore[2]; break;
        case 9: text = g_infoMore[1]; break;
        default: break;
      }
      if (text.length() > 20) text = text.substring(0, 20);
      int tx = (i % 2 == 0) ? 5 : EPD_WIDTH - 5 - (int)text.length() * 6;
      g_canvas.setCursor(tx, i < 2 ? 6 : EPD_HEIGHT - 14);
      g_canvas.print(text);
    }
    g_canvas.drawFastHLine(5, 20, EPD_WIDTH - 10, 1);
    g_canvas.drawFastHLine(5, EPD_HEIGHT - 21, EPD_WIDTH - 10, 1);
    return;
  }

  // Photo, with the time in a small label in the corner.
  if (photoReady()) {
    g_canvas.drawBitmap(0, 0, g_photo, EPD_WIDTH, EPD_HEIGHT, 1);
  } else {
    g_canvas.setFont(&FreeSansBold9pt7b);
    drawCentered("No photo yet", 50);
    drawSmallCentered("Pick one in the Inkfob app, under Faces", 64);
  }
  g_canvas.setFont(&FreeSansBold9pt7b);
  String label = clock + (unread.length() ? "  " + unread : "");
  int w = textWidth(label) + 14;
  g_canvas.fillRoundRect(EPD_WIDTH - w - 4, EPD_HEIGHT - 26, w, 22, 5, 0);
  g_canvas.drawRoundRect(EPD_WIDTH - w - 4, EPD_HEIGHT - 26, w, 22, 5, 1);
  g_canvas.setCursor(EPD_WIDTH - w + 3, EPD_HEIGHT - 10);
  g_canvas.print(label);
}

// ---- To-do list ----------------------------------------------------------
// Kept in the file "todo.list" (sent by the phone app): one item per line,
// "- milk" still to do, "x milk" done. Ticking an item rewrites the file.
static int g_todoSel = 0, g_todoTop = 0;
static const int TODO_ROW_H = 18, TODO_ROWS = 6;
static String clockText();

static void loadTodo() {
  g_todo.clear();
  g_todoDone.clear();
  if (!g_fsReady) return;
  File f = LittleFS.open("/todo.list", "r");
  if (!f) return;
  while (f.available() && g_todo.size() < 40) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    bool done = false;
    if (line.startsWith("x ") || line.startsWith("X ")) { done = true; line = line.substring(2); }
    else if (line.startsWith("[x]") || line.startsWith("[X]")) { done = true; line = line.substring(3); }
    else if (line.startsWith("[ ]")) line = line.substring(3);
    else if (line.startsWith("- ")) line = line.substring(2);
    line.trim();
    if (!line.length()) continue;
    for (size_t i = 0; i < line.length(); i++) if (line[i] < 32 || line[i] > 126) line[i] = '?';
    g_todo.push_back(line.substring(0, 60));
    g_todoDone.push_back(done);
  }
  f.close();
  if (g_todoSel >= (int)g_todo.size()) g_todoSel = max(0, (int)g_todo.size() - 1);
}

static void saveTodo() {
  if (!g_fsReady) return;
  File f = LittleFS.open("/todo.list", "w");
  if (!f) return;
  for (size_t i = 0; i < g_todo.size(); i++) {
    f.print(g_todoDone[i] ? "x " : "- ");
    f.print(g_todo[i]);
    f.print("\n");
  }
  f.close();
}

// Buttons on the list. Returns true when it used the press.
static bool todoPress(int pin) {
  int count = (int)g_todo.size();
  if (count == 0) return false;
  if (pin == PIN_UP && g_todoSel > 0) { g_todoSel--; return true; }
  if (pin == PIN_DOWN && g_todoSel < count - 1) { g_todoSel++; return true; }
  if (pin == PIN_CLICK) {
    g_todoDone[g_todoSel] = !g_todoDone[g_todoSel];
    saveTodo();
    return true;
  }
  return pin == PIN_UP || pin == PIN_DOWN;
}

static void drawTodo(bool asFace) {
  int count = (int)g_todo.size(), left = 0;
  for (int i = 0; i < count; i++) left += g_todoDone[i] ? 0 : 1;
  String right = asFace ? clockText() : String("EXIT = back");
  ClockNow now;
  if (asFace && clockNow(now)) g_homeMinute = now.hour * 60 + now.minute;
  drawHeader(count ? "TO-DO  " + String(left) + " of " + String(count) + " left" : String("TO-DO"), right);
  g_canvas.setTextColor(1);
  if (count == 0) {
    drawSmallCentered("No list yet.", 50);
    drawSmallCentered("Make one in the phone app: Files > To-do list", 66);
    return;
  }
  if (g_todoSel < g_todoTop) g_todoTop = g_todoSel;
  if (g_todoSel >= g_todoTop + TODO_ROWS) g_todoTop = g_todoSel - TODO_ROWS + 1;
  g_canvas.setFont(&FreeSans9pt7b);
  for (int row = 0; row < TODO_ROWS; row++) {
    int idx = g_todoTop + row;
    if (idx >= count) break;
    int y = HEADER_H + row * TODO_ROW_H;
    bool selected = idx == g_todoSel, done = g_todoDone[idx];
    int ink = selected ? 0 : 1;
    if (selected) g_canvas.fillRect(0, y, EPD_WIDTH, TODO_ROW_H, 1);
    g_canvas.drawRect(5, y + 4, 10, 10, ink);                      // the tick box
    if (done) {
      g_canvas.drawLine(7, y + 9, 9, y + 11, ink);
      g_canvas.drawLine(9, y + 11, 12, y + 6, ink);
      g_canvas.drawLine(7, y + 10, 9, y + 12, ink);
      g_canvas.drawLine(9, y + 12, 12, y + 7, ink);
    }
    String text = g_todo[idx];
    while (text.length() > 1 && textWidth(text) > EPD_WIDTH - 26) text.remove(text.length() - 1);
    g_canvas.setTextColor(ink);
    g_canvas.setCursor(21, y + 13);
    g_canvas.print(text);
    if (done) {
      if (!selected) {                                              // fade a finished item to grey
        for (int py = y + 1; py < y + TODO_ROW_H - 1; py++)
          for (int px = 20; px < EPD_WIDTH; px++)
            if ((px + py) & 1) g_canvas.drawPixel(px, py, 0);
      }
      g_canvas.drawFastHLine(20, y + 9, min(textWidth(text) + 3, EPD_WIDTH - 22), ink);   // and strike it out
    }
  }
  g_canvas.setFont(nullptr);
  g_canvas.setTextColor(1);
}

// Expedition mode's home screen: the clock and nothing that needs the phone.
static void drawExpeditionHome() {
  drawHeader("EXPEDITION MODE", "Bluetooth off");
  g_canvas.setTextColor(1);
  ClockNow now;
  if (clockNow(now)) {
    static const char* const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour12 = now.hour % 12;
    if (hour12 == 0) hour12 = 12;
    g_homeMinute = now.hour * 60 + now.minute;
    g_canvas.setFont(&FreeSansBold24pt7b);
    drawCentered(String(hour12) + ":" + (now.minute < 10 ? "0" : "") + String(now.minute) + (now.hour < 12 ? " AM" : " PM"), 64);
    drawSmallCentered(String(DAYS[now.weekday]) + ", " + MONTHS[now.month - 1] + " " + String(now.day), 76);
  } else {
    g_canvas.setFont(&FreeSansBold9pt7b);
    drawCentered("Clock not set", 58);
    drawSmallCentered("Connect to the phone once to set it", 72);
  }
  drawSmallCentered("Sleeps after 1 minute. Any button wakes it.", 96);
  drawSmallCentered("MENU for tools, waypoints and the library", 108);
}

static void drawHome() {
  if (g_expedition) { drawExpeditionHome(); return; }
  if (g_face == 7) { drawTodo(true); return; }
  ClockNow faceNow;
  if (g_face != 0 && clockNow(faceNow)) {
    static const char* const SHORT_DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const SHORT_MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour12 = faceNow.hour % 12;
    if (hour12 == 0) hour12 = 12;
    String clock = String(hour12) + ":" + (faceNow.minute < 10 ? "0" : "") + String(faceNow.minute);
    String shortDate = String(SHORT_DAYS[faceNow.weekday]) + " " + SHORT_MONTHS[faceNow.month - 1] +
                       " " + String(faceNow.day);
    g_homeMinute = faceNow.hour * 60 + faceNow.minute;
    drawFace(clock, faceNow.hour < 12 ? "AM" : "PM", shortDate, shortDate);
    return;
  }
  String right = linkStatus();
  if (g_ancsReady && g_phoneBattery >= 0) right = "iPhone " + String(g_phoneBattery) + "%";
  drawHeader("PAGER", right);
  g_canvas.setTextColor(1);

  ClockNow now;
  bool haveTime = clockNow(now);
  if (!haveTime && !g_ancsReady) {
    drawLinkHelp();
    return;
  }

  String clock = "--:--";
  String ampm;
  String date;
  if (haveTime) {
    static const char* const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
    static const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int hour12 = now.hour % 12;
    if (hour12 == 0) hour12 = 12;
    clock = String(hour12) + ":" + (now.minute < 10 ? "0" : "") + String(now.minute);
    ampm = (now.hour < 12) ? "AM" : "PM";
    date = String(DAYS[now.weekday]) + ", " + MONTHS[now.month - 1] + " " + String(now.day);
    g_homeMinute = now.hour * 60 + now.minute;
  }

  g_canvas.setFont(&FreeSansBold24pt7b);
  int clockW = textWidth(clock);
  g_canvas.setFont(&FreeSansBold9pt7b);
  int ampmW = ampm.length() ? textWidth(ampm) + 6 : 0;
  int x = (EPD_WIDTH - clockW - ampmW) / 2;
  g_canvas.setFont(&FreeSansBold24pt7b);
  g_canvas.setCursor(x, 56);
  g_canvas.print(clock);
  g_canvas.setFont(&FreeSansBold9pt7b);
  g_canvas.setCursor(x + clockW + 6, 56);
  g_canvas.print(ampm);
  if (date.length()) drawCentered(date, 78);

  int unread = unreadCount();
  String summary;
  if (unread > 0) {
    summary = String(unread) + (unread == 1 ? " new notification" : " new notifications");
  } else if (!g_notes.empty()) {
    summary = String((int)g_notes.size()) + " notifications, none new";
  } else {
    summary = "No notifications";
  }
  // Up to three small lines: weather and the next event from the phone app
  // when it has sent them, then the notification count, then what is playing.
  String lines[4];
  int count = 0;
  if (g_infoWeather.length()) lines[count++] = g_infoWeather;
  if (g_infoEvents[0].length()) lines[count++] = g_infoEvents[0];
  lines[count++] = summary;
  if (count < 3 && g_infoEvents[1].length()) lines[count++] = g_infoEvents[1];
  if (count < 3 && g_infoExtra.length()) lines[count++] = g_infoExtra;
  if (count < 3 && g_mediaTitle.length()) {
    String playing = String(g_mediaPlaying ? "Playing: " : "Paused: ") + g_mediaTitle;
    if (g_mediaArtist.length()) playing += " - " + g_mediaArtist;
    lines[count++] = playing;
  }
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  int top = (count >= 3) ? 86 : 90;
  int step = (count >= 3) ? 11 : 16;
  for (int i = 0; i < count && i < 3; i++) drawSmallCentered(fitText(lines[i], EPD_WIDTH - 10), top + i * step);
}

// Breaks text into lines of at most `width` characters, at spaces where it can.
static std::vector<String> wrapText(const String& text, int width) {
  std::vector<String> lines;
  String line;
  int start = 0;
  while (start < (int)text.length()) {
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    String word = text.substring(start, end);
    start = end + 1;
    while ((int)word.length() > width) {
      if (line.length()) {
        lines.push_back(line);
        line = "";
      }
      lines.push_back(word.substring(0, width));
      word = word.substring(width);
    }
    if (line.length() && (int)(line.length() + 1 + word.length()) > width) {
      lines.push_back(line);
      line = "";
    }
    if (word.length()) line += (line.length() ? " " : "") + word;
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

// Everything the app has sent, on one page.
static String clockText();
static void drawToday() {
  drawHeader("TODAY", clockText());
  g_canvas.setTextColor(1);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  std::vector<String> rows;
  const String* facts[] = {&g_infoWeather, &g_infoEvents[0], &g_infoEvents[1], &g_infoExtra,
                           &g_infoMore[0], &g_infoMore[1],   &g_infoMore[2]};
  for (const String* fact : facts) {
    if (fact->length()) rows.push_back(fact->substring(0, 40));
  }
  if (g_quote.length()) {
    if (!rows.empty()) rows.push_back("");
    for (const String& line : wrapText(g_quote, 40)) rows.push_back(line);
  }
  if (rows.empty()) {
    drawSmallCentered("Nothing from the phone yet.", 50);
    drawSmallCentered("Open the Inkfob app to send them.", 64);
    return;
  }
  for (int i = 0; i < (int)rows.size() && i < 10; i++) {
    g_canvas.setCursor(5, HEADER_H + 5 + i * 10);
    g_canvas.print(rows[i]);
  }
}

static void drawMedia() {
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  drawHeader("NOW PLAYING", g_mediaPlayer.substring(0, 16));
  g_canvas.setTextColor(1);

  if (!g_mediaTitle.length()) {
    g_canvas.setFont(&FreeSansBold9pt7b);
    drawCentered(g_ancsReady ? "Nothing playing" : "No phone connected", 52);
  } else {
    g_canvas.setFont(&FreeSansBold9pt7b);
    std::vector<String> lines;
    wrapText(g_mediaTitle, EPD_WIDTH - 8, lines);
    int y = 32;
    for (size_t i = 0; i < lines.size() && i < 2; i++) {
      String line = lines[i];
      if (i == 1 && lines.size() > 2) line = fitText(line + " ...", EPD_WIDTH - 8);
      g_canvas.setCursor(4, y);
      g_canvas.print(line);
      y += 18;
    }
    g_canvas.setFont(&FreeSans9pt7b);
    g_canvas.setCursor(4, y);
    g_canvas.print(fitText(g_mediaArtist, EPD_WIDTH - 8));
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setCursor(4, y + 8);
    g_canvas.print(g_mediaPlaying ? "Playing" : "Paused");
  }

  g_canvas.drawFastHLine(0, 96, EPD_WIDTH, 1);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setCursor(4, 100);
  g_canvas.print("Press: play/pause    EXIT: home");
  g_canvas.setCursor(4, 111);
  g_canvas.print(g_mediaSkipMode ? "Rocker: next/previous   MENU: volume"
                                 : "Rocker: volume   MENU: skip tracks");
}

// A scrolling list in the small font, shared by the main menu and the library.
static const int MENU_ROW_H = 12;
static const int MENU_ROWS = 9;

static void keepVisible(int sel, int& top) {
  if (sel < top) top = sel;
  if (sel >= top + MENU_ROWS) top = sel - MENU_ROWS + 1;
  if (top < 0) top = 0;
}

static void drawRows(int count, int sel, int top, String (*label)(int)) {
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  for (int row = 0; row < MENU_ROWS; row++) {
    int idx = top + row;
    if (idx >= count) break;
    int y = HEADER_H + row * MENU_ROW_H;
    bool selected = (idx == sel);
    if (selected) g_canvas.fillRect(0, y, EPD_WIDTH, MENU_ROW_H, 1);
    g_canvas.setTextColor(selected ? 0 : 1);
    g_canvas.setCursor(6, y + 2);
    g_canvas.print(label(idx).substring(0, 39));
  }
  g_canvas.setTextColor(1);
}

static const int MAIN_MENU_COUNT = 8;

static String mainMenuLabel(int i) {
  switch (i) {
    case 0: {
      int unread = unreadCount();
      return unread ? "Notifications (" + String(unread) + " new)" : String("Notifications");
    }
    case 1: return "Today";
    case 2: {
      int left = 0;
      for (size_t k = 0; k < g_todo.size(); k++) left += g_todoDone[k] ? 0 : 1;
      return left ? "To-do list (" + String(left) + " left)" : String("To-do list");
    }
    case 3: return "Reading  >";
    case 4: return "Phone  >";
    case 5: return "Tools  >";
    case 6: return "Sleep now";
    default: return "Settings";
  }
}

static const int GROUP_COUNTS[2] = {3, 4};

static String groupLabel(int i) {
  static const char* const NAMES[2][4] = {
      {"Survival library", "My files", "Knots and lashings", ""},
      {"Now playing", "Find my phone", "Camera remote", "My card (QR code)"}};
  return NAMES[g_group][i];
}

static void drawGroup() {
  drawHeader(g_group == 0 ? "READING" : "PHONE", "EXIT = back");
  drawRows(GROUP_COUNTS[g_group], g_groupSel[g_group], 0, groupLabel);
}

static void drawMainMenu() {
  drawHeader("MENU", "EXIT = home");
  drawRows(MAIN_MENU_COUNT, g_menuSel, 0, mainMenuLabel);
}

static bool hasBookmark() { return g_markItem >= 0 && g_markItem < Library::itemCount(); }

static int libraryCount() {
  if (g_libLevel == 0) return Library::bookCount() + (hasBookmark() ? 1 : 0);
  if (g_libLevel == 1) return Library::bookGroupCount(g_libBook);
  return Library::groupItemCount(g_libGroup);
}

static String libraryLabel(int i) {
  if (g_libLevel == 0) {
    if (hasBookmark()) {
      if (i == 0) return String("> Continue: ") + Library::itemTitle(g_markItem);
      i--;
    }
    return Library::bookTitle(i);
  }
  if (g_libLevel == 1) return Library::groupTitle(Library::bookFirstGroup(g_libBook) + i);
  return Library::itemTitle(Library::groupFirstItem(g_libGroup) + i);
}

static void drawLibrary() {
  int count = libraryCount();
  int& sel = g_libSel[g_libLevel];
  int& top = g_libTop[g_libLevel];
  if (sel >= count) sel = count - 1;
  if (sel < 0) sel = 0;
  keepVisible(sel, top);

  String left = "LIBRARY";
  if (g_libLevel == 1) left = Library::bookTitle(g_libBook);
  if (g_libLevel == 2) left = Library::groupTitle(g_libGroup);
  left.toUpperCase();
  String right = count ? String(sel + 1) + "/" + String(count) : String("");
  drawHeader(left.substring(0, 32), right);
  if (!Library::ready()) {
    g_canvas.setFont(&FreeSansBold9pt7b);
    drawCentered("Library not available", 60);
    return;
  }
  drawRows(count, sel, top, libraryLabel);
}

// Returns the drawing number if the reader's current page is a drawing, or -1.
static int readerImage(const char** caption = nullptr) {
  static char lines[1][Library::COLS + 1];
  if (Library::readPage(g_readItem, g_readPage, lines, 1) < 1) return -1;
  return Library::parseImageLine(lines[0], caption);
}

static void resetImageView() {
  g_imageZoom = false;
  g_imageY = 0;
  g_imageXStep = 0;
}

// Unpacks one version of a drawing into g_imageBuf (kept until another is needed).
static bool loadImage(int image, bool zoom) {
  if (g_imageLoaded == image && g_imageLoadedZoom == zoom) return true;
  g_imageLoaded = -1;
  Library::Bitmap bm;
  if (!Library::imageBitmap(image, zoom, bm)) return false;
  size_t rawSize = (size_t)((bm.width + 7) / 8) * bm.height;
  if (rawSize > IMAGE_BUF_SIZE) return false;
  tinfl_decompressor* inflator = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  if (!inflator) return false;
  tinfl_init(inflator);
  size_t inSize = bm.packedSize;
  size_t outSize = rawSize;
  tinfl_status status =
      tinfl_decompress(inflator, bm.packed, &inSize, g_imageBuf, g_imageBuf, &outSize,
                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inflator);
  if (status != TINFL_STATUS_DONE || outSize != rawSize) {
    Serial.printf("[library] drawing %d did not unpack (status %d)\n", image, (int)status);
    return false;
  }
  g_imageInfo = bm;
  g_imageLoaded = image;
  g_imageLoadedZoom = zoom;
  return true;
}

static void drawImagePage(int image) {
  if (!loadImage(image, g_imageZoom)) {
    g_canvas.setFont(nullptr);
    g_canvas.setTextSize(1);
    g_canvas.setTextColor(1);
    g_canvas.setCursor(5, HEADER_H + 20);
    g_canvas.print("This drawing could not be shown.");
    return;
  }
  const int w = g_imageInfo.width;
  const int h = g_imageInfo.height;
  const int stride = (w + 7) / 8;

  // Sideways: centre a narrow drawing, or pick one of three positions on a wide one.
  int srcX = 0;
  int dstX = 0;
  if (w <= EPD_WIDTH) {
    dstX = (EPD_WIDTH - w) / 2;
  } else {
    int maxX = w - EPD_WIDTH;
    srcX = (g_imageXStep == 0) ? 0 : (g_imageXStep == 1 ? maxX / 2 : maxX);
  }
  // Up and down: centre a short drawing, or scroll a tall one.
  int maxY = h - IMAGE_VIEW_H;
  if (maxY < 0) maxY = 0;
  if (g_imageY > maxY) g_imageY = maxY;
  if (g_imageY < 0) g_imageY = 0;
  int dstY = HEADER_H + (h < IMAGE_VIEW_H ? (IMAGE_VIEW_H - h) / 2 : 0);

  int rows = h < IMAGE_VIEW_H ? h : IMAGE_VIEW_H;
  int cols = w < EPD_WIDTH ? w : EPD_WIDTH;
  for (int y = 0; y < rows; y++) {
    const uint8_t* row = g_imageBuf + (size_t)(g_imageY + y) * stride;
    for (int x = 0; x < cols; x++) {
      int sx = srcX + x;
      if (row[sx >> 3] & (0x80 >> (sx & 7))) g_canvas.drawPixel(dstX + x, dstY + y, 1);
    }
  }
  if (g_imageY < maxY) {
    // "more below" arrow on a white patch so it stays visible over the drawing
    g_canvas.fillRect(EPD_WIDTH - 15, EPD_HEIGHT - 11, 15, 11, 0);
    g_canvas.fillTriangle(EPD_WIDTH - 12, EPD_HEIGHT - 8, EPD_WIDTH - 4, EPD_HEIGHT - 8,
                          EPD_WIDTH - 8, EPD_HEIGHT - 3, 1);
  }
}

static void drawReader() {
  int pages = Library::itemPageCount(g_readItem);
  if (g_readPage >= pages) g_readPage = pages - 1;
  if (g_readPage < 0) g_readPage = 0;
  String right = String(g_readPage + 1) + "/" + String(pages);

  const char* caption = nullptr;
  int image = readerImage(&caption);
  if (image >= 0) {
    Library::Bitmap zoomInfo;
    bool canZoom = Library::imageBitmap(image, true, zoomInfo);
    if (g_imageZoom && !canZoom) g_imageZoom = false;
    if (g_imageZoom) right = "ZOOM";
    else if (canZoom) right = "+ " + right;  // "+" = press to zoom
    drawHeader(String(caption).substring(0, 39 - (int)right.length() - 1), right);
    drawImagePage(image);
    return;
  }

  drawHeader(String(Library::itemTitle(g_readItem)).substring(0, 39 - (int)right.length() - 1), right);
  static char lines[Library::LINES_PER_PAGE][Library::COLS + 1];
  int n = Library::readPage(g_readItem, g_readPage, lines, Library::LINES_PER_PAGE);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(1);
  for (int i = 0; i < n; i++) {
    g_canvas.setCursor(5, HEADER_H + 5 + i * 10);
    g_canvas.print(lines[i]);
  }
}

static void drawDetail() {
  int idx = findNote(g_detailUid);
  if (idx < 0) {
    g_screen = SCREEN_LIST;
    drawList();
    return;
  }
  const Note& note = g_notes[idx];
  const int visible = bodyLinesVisible();

  useFont(false);
  std::vector<String> lines;
  wrapText(note.body, EPD_WIDTH - 8, lines);
  g_detailLines = (int)lines.size();
  int maxTop = g_detailLines - visible;
  if (maxTop < 0) maxTop = 0;
  if (g_detailTop > maxTop) g_detailTop = maxTop;
  if (g_detailTop < 0) g_detailTop = 0;

  String right;
  if (g_detailLines > visible) {
    int step = visible - 1;
    int pages = (maxTop + step - 1) / step + 1;
    int page = (g_detailTop + step - 1) / step + 1;
    right = String(page) + "/" + String(pages);
  }
  String left = note.app;
  left.toUpperCase();
  drawHeader(left, right);

  g_canvas.setTextColor(1);
  useFont(true);
  printLine(4, HEADER_H + (g_smallText ? 3 : 1), fitText(note.title, EPD_WIDTH - 9), true);

  const int bodyTop = HEADER_H + (g_smallText ? 15 : 19);
  for (int i = 0; i < visible; i++) {
    int line = g_detailTop + i;
    if (line >= g_detailLines) break;
    printLine(4, bodyTop + i * lineHeight(), lines[line], false);
  }
  if (g_detailTop < maxTop) {
    // small "more below" arrow
    g_canvas.fillTriangle(EPD_WIDTH - 12, EPD_HEIGHT - 8, EPD_WIDTH - 4, EPD_HEIGHT - 8,
                          EPD_WIDTH - 8, EPD_HEIGHT - 3, 1);
  }
}

static const int SETTINGS_COUNT = 15;

static String minutesLabel(int minutes) {
  if (minutes >= 60) return String(minutes / 60) + (minutes == 60 ? " hour" : " hours");
  return String(minutes) + " min";
}

static String settingsLabel(int i) {
  switch (i) {
    case 0: return String("Text size: ") + (g_smallText ? "Small" : "Large");
    case 1: return String("Flip screen: ") + (g_flip ? "On" : "Off");
    case 2: return String("Fast refresh: ") + (g_fastRefresh ? "On" : "Off");
    case 3: return String("Mirror screen: ") + (g_mirror ? "On" : "Off");
    case 4: {
      int minutes = SLEEP_MINUTES[g_sleepChoice];
      return String("Sleep after: ") + (minutes ? minutesLabel(minutes) : String("Never"));
    }
    case 5: {
      int minutes = CHECK_MINUTES[g_checkChoice];
      return String("Asleep, check phone: ") +
             (minutes ? "every " + minutesLabel(minutes) : String("Off"));
    }
    case 6: return String("Sleep screen: ") + (g_sleepFace ? "My card" : "Status");
    case 7: return String("Home face: ") + FACE_NAMES[g_face];
    case 8: {
      int seconds = POPUP_SECONDS[g_popupChoice];
      if (!seconds) return String("Alerts stay: until dismissed");
      return String("Alerts stay: ") + (seconds < 60 ? String(seconds) + " sec" : minutesLabel(seconds / 60));
    }
    case 9: return String("Controls: ") + (g_twoButton ? "Two buttons" : "Wheel");
    case 10: return String("Reading: ") + READ_NAMES[g_readTurn];
    case 11: return String("Expedition mode: ") + (g_expedition ? "On" : "Off");
    case 12: return "Status";
    case 13: return "Clear list";
    default: return "Forget phone";
  }
}

static void drawSettings() {
  drawHeader("SETTINGS", "EXIT = back");
  keepVisible(g_settingsSel, g_settingsTop);
  drawRows(SETTINGS_COUNT, g_settingsSel, g_settingsTop, settingsLabel);
}

// A plain-language readout of what the pager thinks is going on, so problems
// can be diagnosed from the screen without a computer.
static String spanText(uint32_t seconds) {
  if (seconds < 60) return String(seconds) + "s";
  if (seconds < 3600) return String(seconds / 60) + "m" + String(seconds % 60) + "s";
  return String(seconds / 3600) + "h" + String((seconds % 3600) / 60) + "m";
}

// One line per recent phone link: when it ended, how long it lasted, how far
// it got, and why it ended. The pattern shows what kind of fault it is.
static void drawLinkLog() {
  drawHeader("LINK LOG", FIRMWARE_VERSION);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(1);
  static const char* const STAGES[] = {"linked", "paired", "notif"};
  String now;
  if (g_connected) {
    now = "Now: up " + spanText((millis() - g_linkStartMs) / 1000) + ", " + STAGES[g_linkStage > 2 ? 2 : g_linkStage];
  } else {
    now = "Now: no phone";
    if (g_dropCount) now += ", for " + spanText((millis() - g_dropAtMs) / 1000);
  }
  g_canvas.setCursor(6, HEADER_H + 5);
  g_canvas.print(now);
  g_canvas.setCursor(6, HEADER_H + 19);
  g_canvas.print("ended    lasted   got to  why");
  int total = g_linkLogCount;
  int shown = total < LINK_LOG_SIZE ? total : LINK_LOG_SIZE;
  for (int i = 0; i < shown; i++) {
    const LinkRecord& rec = g_linkLog[(total - 1 - i) % LINK_LOG_SIZE];
    const char* why = rec.reason == 0x08 ? "range" : rec.reason == 0x13 ? "phone" :
                      rec.reason == 0x16 ? "pager" : rec.reason == 0x3D ? "crypt" :
                      rec.reason == 0x3E ? "nolink" : rec.reason == 0x22 ? "noreply" : "other";
    char line[48];
    snprintf(line, sizeof(line), "-%-7s %-8s %-6s  %s %02X", spanText((millis() - rec.endMs) / 1000).c_str(),
             spanText(rec.seconds).c_str(), STAGES[rec.stage > 2 ? 2 : rec.stage], why, rec.reason);
    g_canvas.setCursor(6, HEADER_H + 31 + i * 12);
    g_canvas.print(line);
  }
  if (total == 0) {
    g_canvas.setCursor(6, HEADER_H + 31);
    g_canvas.print("No drops since power-on");
  }
}

static void drawStatus() {
  if (g_statusPage == 1) {
    drawLinkLog();
    return;
  }
  drawHeader("STATUS", FIRMWARE_VERSION);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(1);

  String lines[7];
  lines[0] = String("Screen type: ") + (Epd::panel() == PANEL_V12 ? "B" : "A") +
             " (auto says " + (Epd::detected() == PANEL_V12 ? "B" : "A") + ")";
  if (!g_connected) {
    lines[1] = "Phone: not connected";
  } else if (!g_secured) {
    lines[1] = "Phone: connected, not paired";
  } else {
    lines[1] = "Phone: connected and paired";
  }
  lines[2] = "Phones remembered: " + String(NimBLEDevice::getNumBonds());
  if (g_ancsReady) {
    lines[3] = "Notifications: receiving";
  } else if (g_secured) {
    lines[3] = "Notifications: asking, try " + String((int)g_discoverTries);
  } else {
    lines[3] = "Notifications: waiting";
  }
  lines[4] = "Stored: " + String((int)g_notes.size()) + "   Loading: " +
             String((int)g_pending.size() + (g_fetching ? 1 : 0));
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  lines[5] = String("Looking for phone: ") + (g_connected ? "no need" : (adv && adv->isAdvertising()) ? "yes" : "NO");
  lines[5] += "   Links: " + String((int)g_connectCount);
  if (g_dropCount == 0) {
    lines[6] = "Link drops: none since power-on";
  } else {
    int code = g_dropReason & 0xFF;
    const char* why = code == 0x08 ? "out of range" : code == 0x13 ? "phone ended it" :
                      code == 0x16 ? "pager ended it" : code == 0x3D ? "bad encryption" :
                      code == 0x3E ? "never settled" : "other";
    lines[6] = "Drops: " + String((int)g_dropCount) + ", last " + why + " (" + String(code, HEX) +
               ") " + String((int)((millis() - g_dropAtMs) / 60000)) + "m ago";
  }
  for (int i = 0; i < 7; i++) {
    g_canvas.setCursor(6, HEADER_H + 5 + i * 14);
    g_canvas.print(lines[i]);
  }
}

// ------------------------------------------------------------- my files
// Text files sent from the phone app, kept in the board's own storage.

static std::vector<String> g_fileNames;
static int g_fileSel = 0;
static int g_fileTop = 0;
static File g_noteFile;
static String g_noteName;
static std::vector<uint32_t> g_notePages;   // where each page starts in the file
static int g_notePage = 0;
static uint32_t g_noteSize = 0;              // what the page list was worked out for
static uint16_t g_noteLayout = 0;

// Reads a file through a small buffer: one character at a time straight from
// storage is what made big books slow to open.
struct NoteSrc {
  File* f = nullptr;
  uint8_t buf[512];
  uint32_t base = 0;
  int len = 0;
  int at = 0;
  void open(File& file, uint32_t pos) { f = &file; base = pos; len = 0; at = 0; }
  uint32_t position() const { return base + at; }
  bool available() {
    if (at < len) return true;
    base += len;
    at = 0;
    f->seek(base);
    len = (int)f->read(buf, sizeof(buf));
    if (len < 0) len = 0;
    return len > 0;
  }
  int read() { return available() ? buf[at++] : -1; }
  void seek(uint32_t pos) {
    if (pos >= base && pos <= base + (uint32_t)len) { at = (int)(pos - base); return; }
    base = pos; len = 0; at = 0;
  }
};
static NoteSrc g_noteSrc;

static int noteCols() { return g_readTurn ? 19 : 40; }
static int noteRows() { return g_readTurn ? (EPD_WIDTH - HEADER_H - 6) / 10 : 10; }

static void refreshFileList() {
  g_fileNames.clear();
  if (!g_fsReady) return;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = String(f.name());
    if (!name.endsWith(".img") && !name.endsWith(".list")) g_fileNames.push_back(name);  // not notes
  }
  std::sort(g_fileNames.begin(), g_fileNames.end(),
            [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  if (g_fileSel >= (int)g_fileNames.size()) g_fileSel = max(0, (int)g_fileNames.size() - 1);
}

// Flashcards: a file whose name ends in ".cards", one "question | answer" per line.
static std::vector<uint32_t> g_deckLines;
static String g_deckName;
static int g_deckAt = 0;
static bool g_deckBack = false;

static bool openDeck(const String& name) {
  File f = LittleFS.open("/" + name, "r");
  if (!f) return false;
  g_deckLines.clear();
  uint32_t at = 0;
  bool lineHasText = false;
  while (f.available() && g_deckLines.size() < 500) {
    int c = f.read();
    if (c == '\n') {
      if (lineHasText) g_deckLines.push_back(at);
      at = f.position();
      lineHasText = false;
    } else if (c > ' ') {
      lineHasText = true;
    }
  }
  if (lineHasText) g_deckLines.push_back(at);
  f.close();
  g_deckName = name;
  g_deckAt = 0;
  g_deckBack = false;
  return !g_deckLines.empty();
}

static void drawDeck() {
  String title = g_deckName.substring(0, g_deckName.length() - 6);
  drawHeader(title.substring(0, 28), String(g_deckAt + 1) + "/" + String((int)g_deckLines.size()));
  g_canvas.setTextColor(1);
  String line;
  File f = LittleFS.open("/" + g_deckName, "r");
  if (f) {
    f.seek(g_deckLines[g_deckAt]);
    line = f.readStringUntil('\n');
    f.close();
  }
  line.trim();
  int bar = line.indexOf('|');
  String front = bar >= 0 ? line.substring(0, bar) : line;
  String back = bar >= 0 ? line.substring(bar + 1) : String("(no answer on this line)");
  front.trim();
  back.trim();
  std::vector<String> rows = wrapText(g_deckBack ? back : front, 24);
  g_canvas.setFont(g_deckBack ? &FreeSans9pt7b : &FreeSansBold9pt7b);
  int count = min((int)rows.size(), 4);
  int y = 58 - (count - 1) * 10;
  for (int i = 0; i < count; i++) drawCentered(rows[i], y + i * 20);
  drawSmallCentered(g_deckBack ? "Answer     Wheel: next card" : "Press: show answer", EPD_HEIGHT - 12);
}

static bool photoReady() {
  if (!g_photoTried) {
    g_photoTried = true;
    g_photoOk = false;
    if (g_fsReady) {
      File f = LittleFS.open("/face.img", "r");
      if (f && f.size() == PHOTO_BYTES) g_photoOk = (f.read(g_photo, PHOTO_BYTES) == (int)PHOTO_BYTES);
      if (f) f.close();
    }
  }
  return g_photoOk;
}

// Reads the next screen line (up to `cols` characters, broken at a space).
static bool noteNextLine(NoteSrc& f, char* out, int cols) {
  int n = 0;
  int lastSpace = -1;
  uint32_t afterSpace = 0;
  bool any = false;
  while (f.available()) {
    int c = f.read();
    any = true;
    if (c == '\r') continue;
    if (c == '\n') break;
    if (c == '\t') c = ' ';
    if (c < 32 || c > 126) c = '?';
    if (n == cols) {
      if (c == ' ') break;
      if (lastSpace > 0) {
        f.seek(afterSpace);
        n = lastSpace;
      } else {
        f.seek(f.position() - 1);
      }
      break;
    }
    if (c == ' ') {
      lastSpace = n;
      afterSpace = f.position();
    }
    out[n++] = (char)c;
  }
  out[n] = 0;
  return any;
}

static String noteKey(const String& name) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < name.length(); i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
  char key[12];
  snprintf(key, sizeof(key), "n%08x", (unsigned)h);
  return String(key);
}

static String noteSpotKey(const String& name) {
  String key = noteKey(name);
  key[0] = 'o';
  return key;
}

// Shown while a big file is being split into pages, so it does not look frozen.
static void showOpening(const String& name, int percent) {
  g_canvas.fillScreen(0);
  drawHeader("OPENING", String(percent) + "%");
  g_canvas.setTextColor(1);
  drawSmallCentered(name.substring(0, 38), 44);
  int w = EPD_WIDTH - 60;
  g_canvas.drawRect(30, 62, w, 14, 1);
  g_canvas.fillRect(32, 64, (w - 4) * percent / 100, 10, 1);
  drawSmallCentered("Working out the pages...", 92);
  Epd::show(g_canvas.getBuffer(), false);
  g_fastCount++;
}

static void saveNoteSpot() {
  if (g_noteName.length() && g_notePage < (int)g_notePages.size())
    g_prefs.putUInt(noteSpotKey(g_noteName).c_str(), g_notePages[g_notePage]);
}

static bool openNote(const String& name) {
  if (g_noteFile) g_noteFile.close();
  g_noteFile = LittleFS.open("/" + name, "r");
  if (!g_noteFile) return false;
  int cols = noteCols();
  int rows = noteRows();
  uint32_t size = g_noteFile.size();
  uint16_t layout = (uint16_t)(cols << 8 | rows);
  bool known = name == g_noteName && size == g_noteSize && layout == g_noteLayout &&
               !g_notePages.empty();
  g_noteName = name;
  if (!known) {
    stayAwake(true);
    g_notePages.clear();
    bool big = size > 30000;
    uint32_t shownAt = millis();
    if (big) showOpening(name, 0);
    char line[41];
    int lines = 0;
    uint32_t at = 0;
    g_noteSrc.open(g_noteFile, 0);
    while (true) {
      if (lines % rows == 0) at = g_noteSrc.position();
      if (!noteNextLine(g_noteSrc, line, cols)) break;
      if (lines % rows == 0) g_notePages.push_back(at);
      lines++;
      if (big && (lines & 255) == 0 && millis() - shownAt > 1500) {
        showOpening(name, (int)((uint64_t)at * 100 / size));
        shownAt = millis();
      }
    }
    if (g_notePages.empty()) g_notePages.push_back(0);
    g_noteSize = size;
    g_noteLayout = layout;
  }
  // The place is kept as a spot in the file, so it survives a change of layout.
  String spotKey = noteSpotKey(name);
  g_notePage = 0;
  if (g_prefs.isKey(spotKey.c_str())) {
    uint32_t spot = g_prefs.getUInt(spotKey.c_str(), 0);
    for (int i = 0; i < (int)g_notePages.size() && g_notePages[i] <= spot; i++) g_notePage = i;
  } else if (!g_readTurn) {
    g_notePage = g_prefs.getInt(noteKey(name).c_str(), 0);   // saved by older firmware
  }
  if (g_notePage >= (int)g_notePages.size()) g_notePage = 0;
  return true;
}

// The reading layout changed: split the open file into pages again.
static void setReadTurn(uint8_t turn) {
  if (g_screen == SCREEN_NOTE) saveNoteSpot();
  g_readTurn = turn % 3;
  g_prefs.putUChar("readTurn", g_readTurn);
  if (g_screen == SCREEN_NOTE && g_noteName.length()) openNote(String(g_noteName));
  g_forceFull = true;
}

static String fileLabel(int i) { return g_fileNames[i]; }

static void drawFiles() {
  drawHeader("MY FILES", "EXIT = back");
  if (g_fileNames.empty()) {
    g_canvas.setTextColor(1);
    drawSmallCentered("Nothing here yet.", 50);
    drawSmallCentered("Send notes or books from the phone app.", 66);
    return;
  }
  keepVisible(g_fileSel, g_fileTop);
  drawRows((int)g_fileNames.size(), g_fileSel, g_fileTop, fileLabel);
}

// Tall layout: the same page drawn with the screen turned on its side.
static void drawNoteTall() {
  g_canvas.setRotation(g_readTurn == 2 ? 3 : 1);
  int w = g_canvas.width();
  int cols = noteCols();
  String right = String(g_notePage + 1) + "/" + String((int)g_notePages.size());
  g_canvas.fillRect(0, 0, w, HEADER_H, 1);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(0);
  g_canvas.setCursor(3, 3);
  g_canvas.print(g_noteName.substring(0, max(0, cols - (int)right.length() - 1)));
  g_canvas.setCursor(w - 3 - (int)right.length() * 6, 3);
  g_canvas.print(right);
  g_canvas.setTextColor(1);
  if (g_noteFile) {
    g_noteSrc.open(g_noteFile, g_notePages[g_notePage]);
    char line[41];
    int rows = noteRows();
    for (int i = 0; i < rows; i++) {
      if (!noteNextLine(g_noteSrc, line, cols)) break;
      g_canvas.setCursor(4, HEADER_H + 4 + i * 10);
      g_canvas.print(line);
    }
  }
  g_canvas.setRotation(0);
}

static void drawNote() {
  if (g_readTurn) { drawNoteTall(); return; }
  String right = String(g_notePage + 1) + "/" + String((int)g_notePages.size());
  drawHeader(g_noteName.substring(0, 39 - (int)right.length() - 1), right);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(1);
  if (!g_noteFile) return;
  g_noteSrc.open(g_noteFile, g_notePages[g_notePage]);
  char line[41];
  for (int i = 0; i < 10; i++) {
    if (!noteNextLine(g_noteSrc, line, 40)) break;
    g_canvas.setCursor(5, HEADER_H + 5 + i * 10);
    g_canvas.print(line);
  }
}

static void drawUpdate() {
  drawHeader("UPDATING", FIRMWARE_VERSION);
  g_canvas.setTextColor(1);
  int percent = g_xferSize ? (int)((uint64_t)g_xferGot * 100 / g_xferSize) : 0;
  g_canvas.setFont(&FreeSansBold24pt7b);
  drawCentered(String(percent) + "%", 64);
  drawSmallCentered("Installing new firmware from the phone", 84);
  drawSmallCentered("Keep the phone close. Do not switch off.", 100);
}

// ---- Waypoints -----------------------------------------------------------
// The phone app sends its position whenever it sends anything else. The pager
// keeps the latest one (the "fix") and up to eight saved spots, all stored so
// they survive sleep, restarts and a dead phone.
struct Fix {
  double lat, lon;
  float accuracy;      // metres
  int64_t at;          // pager clock, seconds (0 = clock was not set)
  uint8_t ok;
};
struct Waypoint {
  char name[16];
  double lat, lon;
  int64_t at;
};
static const int WAYPOINT_MAX = 8;
static Fix g_fix = {};
static Waypoint g_waypoints[WAYPOINT_MAX];
static int g_waypointCount = 0;
static int g_wpSel = 0, g_wpTop = 0, g_wpOpen = 0;
static bool g_wpArmed = false;
static String g_wpNote;

static int64_t clockSeconds() {
  if (!g_timeKnown) return 0;
  return g_timeBase + (uint32_t)(millis() - g_timeBaseMs) / 1000;
}

static void loadWaypoints() {
  if (g_prefs.getBytesLength("fix") == sizeof(g_fix)) g_prefs.getBytes("fix", &g_fix, sizeof(g_fix));
  size_t len = g_prefs.getBytesLength("wpts");
  if (len > 0 && len % sizeof(Waypoint) == 0 && len <= sizeof(g_waypoints)) {
    g_prefs.getBytes("wpts", g_waypoints, len);
    g_waypointCount = (int)(len / sizeof(Waypoint));
  }
}

static void saveWaypoints() {
  if (g_waypointCount == 0) g_prefs.remove("wpts");
  else g_prefs.putBytes("wpts", g_waypoints, g_waypointCount * sizeof(Waypoint));
}

// "lat,lon,accuracy in metres,age in seconds"
static void setFix(const String& text) {
  double v[4] = {0, 0, 0, 0};
  int n = 0, from = 0;
  while (n < 4 && from <= (int)text.length()) {
    int comma = text.indexOf(',', from);
    if (comma < 0) comma = text.length();
    v[n++] = text.substring(from, comma).toDouble();
    from = comma + 1;
  }
  if (n < 2 || v[0] < -90 || v[0] > 90 || v[1] < -180 || v[1] > 180) return;
  if (v[0] == 0 && v[1] == 0) return;
  g_fix.lat = v[0];
  g_fix.lon = v[1];
  g_fix.accuracy = (float)v[2];
  int64_t now = clockSeconds();
  g_fix.at = now ? now - (int64_t)v[3] : 0;
  g_fix.ok = 1;
  g_prefs.putBytes("fix", &g_fix, sizeof(g_fix));
}

// Saves the latest fix under a name. Returns false when there is no fix or no room.
static bool addWaypoint(const String& wanted) {
  if (!g_fix.ok || g_waypointCount >= WAYPOINT_MAX) return false;
  String name = wanted;
  name.trim();
  if (!name.length()) {
    int n = 1;
    for (bool clash = true; clash; ) {
      name = "Mark " + String(n++);
      clash = false;
      for (int i = 0; i < g_waypointCount; i++) clash = clash || name == g_waypoints[i].name;
    }
  }
  for (size_t i = 0; i < name.length(); i++) if (name[i] < 32 || name[i] > 126) name[i] = '?';
  Waypoint& w = g_waypoints[g_waypointCount++];
  memset(&w, 0, sizeof(w));
  strncpy(w.name, name.c_str(), sizeof(w.name) - 1);
  w.lat = g_fix.lat;
  w.lon = g_fix.lon;
  w.at = g_fix.at;
  saveWaypoints();
  return true;
}

static void deleteWaypoint(int i) {
  if (i < 0 || i >= g_waypointCount) return;
  for (int k = i; k < g_waypointCount - 1; k++) g_waypoints[k] = g_waypoints[k + 1];
  g_waypointCount--;
  saveWaypoints();
}

// Straight-line distance in metres and compass bearing in degrees from a to b.
static void rangeTo(double lat1, double lon1, double lat2, double lon2, double& metres, double& bearing) {
  const double R = 6371000.0, D = 0.017453292519943295;
  double p1 = lat1 * D, p2 = lat2 * D, dp = (lat2 - lat1) * D, dl = (lon2 - lon1) * D;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  metres = 2 * R * atan2(sqrt(a), sqrt(1 - a));
  bearing = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / D;
  if (bearing < 0) bearing += 360;
}

static String rangeText(const Waypoint& w) {
  if (!g_fix.ok) return "";
  double m, b;
  rangeTo(g_fix.lat, g_fix.lon, w.lat, w.lon, m, b);
  static const char* const POINTS[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  double feet = m * 3.28084, miles = m / 1609.344;
  if (feet < 60) return "here";
  String d = feet < 1000 ? String((int)(feet / 10 + 0.5) * 10) + " ft"
           : miles < 100 ? String(miles, 1) + " mi" : String((int)(miles + 0.5)) + " mi";
  return d + " " + POINTS[(int)((b + 22.5) / 45) % 8];
}

static String agoText(int64_t at) {
  int64_t now = clockSeconds();
  if (!at || !now || now < at) return "";
  int64_t s = now - at;
  if (s < 90) return "just now";
  if (s < 5400) return String((int)(s / 60)) + " min ago";
  if (s < 48 * 3600) return String((int)(s / 3600)) + " hr ago";
  return String((int)(s / 86400)) + " days ago";
}

// Applies a setting sent by the app ("key=value").
static void applyAppSetting(const String& pair) {
  int eq = pair.indexOf('=');
  if (eq < 1) return;
  String key = pair.substring(0, eq);
  String value = pair.substring(eq + 1);
  int number = value.toInt();
  if (key == "small") {
    g_smallText = number != 0;
    g_prefs.putBool("small", g_smallText);
  } else if (key == "flip") {
    g_flip = number != 0;
    g_prefs.putBool("flip", g_flip);
    Epd::setFlip(g_flip);
    g_forceFull = true;
  } else if (key == "fast") {
    g_fastRefresh = number != 0;
    g_prefs.putBool("fast", g_fastRefresh);
  } else if (key == "sleep" && number >= 0 && number < SLEEP_CHOICES) {
    g_sleepChoice = (uint8_t)number;
    g_prefs.putUChar("sleepSel", g_sleepChoice);
  } else if (key == "check" && number >= 0 && number < CHECK_CHOICES) {
    g_checkChoice = (uint8_t)number;
    g_prefs.putUChar("checkSel", g_checkChoice);
  } else if (key == "face") {
    g_sleepFace = number ? 1 : 0;
    g_prefs.putUChar("sleepFace", g_sleepFace);
  } else if (key == "home" && number >= 0 && number < FACE_COUNT) {
    g_face = (uint8_t)number;
    g_prefs.putUChar("homeFace", g_face);
    g_photoTried = false;
    g_forceFull = true;
  } else if (key == "btn2") {
    g_twoButton = number != 0;
    g_prefs.putBool("twoButton", g_twoButton);
  } else if (key == "exped") {
    setExpedition(number != 0);
  } else if (key == "read" && number >= 0 && number < 3) {
    setReadTurn((uint8_t)number);
  } else if (key == "popup" && number >= 0 && number < POPUP_CHOICES) {
    g_popupChoice = (uint8_t)number;
    g_prefs.putUChar("popupSel", g_popupChoice);
  } else if (key == "corners" && value.length() == 4) {
    for (int i = 0; i < 4; i++) g_corners[i] = (uint8_t)constrain(value[i] - '0', 0, 9);
    g_prefs.putString("corners", value);
  } else if (key == "name") {
    g_cardName = value.substring(0, 40);
    g_prefs.putString("cName", g_cardName);
  } else if (key == "line1") {
    g_cardLine1 = value.substring(0, 40);
    g_prefs.putString("cLine1", g_cardLine1);
  } else if (key == "line2") {
    g_cardLine2 = value.substring(0, 40);
    g_prefs.putString("cLine2", g_cardLine2);
  } else if (key == "qr") {
    g_cardQr = value.substring(0, 100);
    g_prefs.putString("cQr", g_cardQr);
    g_cardQrStale = true;
  }
  g_lastActivityMs = millis();
}

// The main loop's share of the app link.
static void serviceAppLink() {
  static Screen screenBeforeUpdate = SCREEN_HOME;
  static int shownPercent = -1;
  static uint32_t nextAskMs = 0;
  static bool asked = false;

  AppCommand c;
  while (xQueueReceive(g_appMainQueue, &c, 0) == pdTRUE) {
    String text = String(c.text);
    if (c.cmd == 'S') {
      applyAppSetting(text);
      markDirty(200);
    } else if (c.cmd == 'I') {
      // weather on the first line, then up to two upcoming events
      String parts[4];
      int part = 0;
      for (size_t i = 0; i < text.length() && part < 4; i++) {
        if (text[i] == '\n') part++;
        else parts[part] += text[i];
      }
      g_infoWeather = parts[0];
      g_infoEvents[0] = parts[1];
      g_infoEvents[1] = parts[2];
      g_infoExtra = parts[3];
      if (g_screen == SCREEN_HOME) markDirty(300);
    } else if (c.cmd == 'J') {
      String parts[3];
      int part = 0;
      for (size_t i = 0; i < text.length() && part < 3; i++) {
        if (text[i] == '\n') part++;
        else parts[part] += text[i];
      }
      for (int i = 0; i < 3; i++) g_infoMore[i] = parts[i];
      if (g_screen == SCREEN_HOME || g_screen == SCREEN_TODAY) markDirty(300);
    } else if (c.cmd == 'Q') {
      g_quote = text;
      if (g_screen == SCREEN_TODAY) markDirty(300);
    } else if (c.cmd == 'W') {
      setFix(text);
      if (g_screen == SCREEN_WAYPOINTS || g_screen == SCREEN_WAYPOINT) markDirty(300);
    } else if (c.cmd == 'M') {
      addWaypoint(text);
      if (g_screen == SCREEN_WAYPOINTS) markDirty(300);
    }
  }

  if (g_filesChanged) {
    g_filesChanged = false;
    refreshFileList();
    loadTodo();
    g_photoTried = false;
    if (g_screen == SCREEN_FILES || g_screen == SCREEN_TODO || (g_screen == SCREEN_HOME && g_face == 7)) markDirty(200);
    if (g_screen == SCREEN_HOME && g_face == 5) {
      g_forceFull = true;
      markDirty(200);
    }
  }

  // Firmware install: show progress, and keep the screen on it.
  if (g_xferKind == 2) {
    if (g_screen != SCREEN_UPDATE) {
      screenBeforeUpdate = g_screen;
      g_screen = SCREEN_UPDATE;
      shownPercent = -1;
    }
    int percent = g_xferSize ? (int)((uint64_t)g_xferGot * 100 / g_xferSize) / 5 * 5 : 0;
    if (percent != shownPercent) {
      shownPercent = percent;
      markDirty(1);
    }
    g_lastActivityMs = millis();
  } else if (g_screen == SCREEN_UPDATE) {
    g_screen = screenBeforeUpdate;
    markDirty(0);
  }

  // Ask the app for fresh weather and calendar: soon after it starts
  // listening, then every 30 minutes. The ask also wakes the app up.
  if (!g_appListening || !g_connected) {
    asked = false;
    return;
  }
  if (!asked && millis() - g_appListenAtMs > 3000) {
    asked = true;
    nextAskMs = millis() + 30UL * 60UL * 1000UL;
    appNotifyText('R', "");
  } else if (asked && (int32_t)(millis() - nextAskMs) >= 0) {
    nextAskMs = millis() + 30UL * 60UL * 1000UL;
    appNotifyText('R', "");
  }
}

static String clockText();
static void openMainMenu();

// ------------------------------------------------------------------ tools

// QR code maker that ships inside the chip's system libraries.
extern "C" {
bool qrcodegen_encodeText(const char* text, uint8_t tempBuffer[], uint8_t qrcode[], int ecl,
                          int minVersion, int maxVersion, int mask, bool boostEcl);
int qrcodegen_getSize(const uint8_t qrcode[]);
bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y);
}

static const int TOOLS_COUNT = 9;
static const int GAMES_COUNT = 7;
static int g_toolsSel = 0, g_toolsTop = 0, g_gamesSel = 0;

// Pomodoro: 25 minutes of work, then a 5 minute break, over and over.
static int g_pomoPhase = 0;         // 0 not started, 1 work, 2 break
static bool g_pomoRunning = false;
static uint32_t g_pomoEndMs = 0;
static int g_pomoLeftSec = 25 * 60, g_pomoDone = 0, g_pomoShownKey = -1;
static bool g_pomoBlink = false;

static int g_tally = 0;
static int g_tipBill = 40, g_tipPercent = 20, g_tipPeople = 2, g_tipField = 0;
static int g_convPair = 0, g_convValue = 10;
static bool g_convReverse = false;
static uint32_t g_findAtMs = 0;
static bool g_findSent = false;
static int g_morseAt = 0;
static bool g_morseQuiz = false, g_morseShown = true;

// Hangman
static const char* const HANG_WORDS[] = {
    "KEYCHAIN", "PAGER",   "BATTERY", "ROCKET",   "PUZZLE",  "GALAXY",  "WIZARD",   "JUNGLE",
    "OXYGEN",   "RHYTHM",  "PIRATE",  "COFFEE",   "THUNDER", "LANTERN", "MAGNET",   "PYRAMID",
    "COMPASS",  "ORCHARD", "WHISPER", "VOLCANO",  "BLANKET", "CACTUS",  "DOLPHIN",  "FOSSIL",
    "GLACIER",  "HARBOR",  "ISLAND",  "JOURNEY",  "KITCHEN", "LIBRARY", "MONSOON",  "NEUTRON",
    "OCTOPUS",  "PENGUIN", "QUARTZ",  "RAINBOW",  "SPHINX",  "TORNADO", "UMBRELLA", "VOYAGE",
    "WALRUS",   "ZEPPELIN", "BISCUIT", "CHIMNEY", "DRAGON",  "ECLIPSE", "FALCON",   "GUITAR",
    "HAMMOCK",  "IGLOO",   "JIGSAW",  "KAYAK",    "LOBSTER", "MAMMOTH", "NOODLE",   "OYSTER",
};
static const int HANG_COUNT = sizeof(HANG_WORDS) / sizeof(HANG_WORDS[0]);
static const char* g_hangWord = nullptr;
static uint32_t g_hangGuessed = 0;
static int g_hangWrong = 0, g_hangLetter = 4, g_hangWins = 0;

// Minesweeper: 13 x 6 squares, 12 mines.
static const int MS_COLS = 13, MS_ROWS = 6, MS_CELL = 17, MS_MINES = 12;
static uint8_t g_msCell[MS_COLS * MS_ROWS];  // bit 7 mine, bit 6 open, low bits = neighbours
static int g_msX = 0, g_msY = 0, g_msState = 0;  // 0 not started, 1 playing, 2 lost, 3 won
static bool g_msAcross = true;

// Countdown timer
static int g_timerSetSec = 300;
static int g_timerLeftSec = 300;  // what is left while paused or not started
static bool g_timerRunning = false;
static uint32_t g_timerEndMs = 0;
static bool g_timerDone = false;
static uint32_t g_timerDoneAtMs = 0;
static bool g_timerBlinking = false;
static int g_timerShownKey = -1;

// Stopwatch
static bool g_swRunning = false;
static uint32_t g_swStartMs = 0;
static uint32_t g_swAccumMs = 0;
static int g_swShownSec = -1;

// Decision makers
static int g_oracleAnswer = -1;
static int g_oracleCount = 0;
static const int DICE_SIDES[] = {4, 6, 8, 10, 12, 20, 100, 0};  // 0 = two six-sided dice
static const int DICE_TYPES = 8;
static int g_diceType = 1;
static int g_diceA = 0, g_diceB = 0;
static int g_diceCount = 0;
static int g_coinSide = -1;
static int g_coinCount = 0, g_coinHeads = 0;

static const char* const ORACLE_ANSWERS[] = {
    "Yes, go for it",   "No way",        "Ask again later",    "Looks good",
    "Not today",        "Absolutely",    "Doubtful",           "Sleep on it",
    "Signs say yes",    "Bad idea",      "Do it",              "Wait a day",
    "Trust your gut",   "Probably",      "Probably not",       "Without a doubt",
    "Not a chance",     "Flip a coin",   "Yes, but carefully", "Only if it's fun",
};
static const int ORACLE_COUNT = sizeof(ORACLE_ANSWERS) / sizeof(ORACLE_ANSWERS[0]);

// Snake: a 31 x 13 board of 8-pixel squares under the top bar.
static const int SN_COLS = 31, SN_ROWS = 13, SN_CELL = 8, SN_MAX = 240;
static const uint32_t SN_STEP_MS = 650;
static uint8_t g_snX[SN_MAX], g_snY[SN_MAX];  // [0] is the head
static int g_snLen = 0, g_snDir = 0, g_snTurn = 0;  // dir: 0 right, 1 down, 2 left, 3 up
static int g_snFoodX = 0, g_snFoodY = 0, g_snBest = 0;
static bool g_snAlive = false, g_snPaused = true;
static uint32_t g_snNextMs = 0;

// 2048: each square holds a power of two (0 = empty).
static uint8_t g_tiles[16];
static uint32_t g_tScore = 0, g_tBest = 0;
static bool g_tOver = false, g_tStarted = false;

static bool isToolScreen() {
  return g_screen == SCREEN_GAMES || g_screen == SCREEN_POMO || g_screen == SCREEN_TALLY ||
         g_screen == SCREEN_TIP || g_screen == SCREEN_CONVERT || g_screen == SCREEN_FIND ||
         g_screen == SCREEN_MORSE || g_screen == SCREEN_HANGMAN || g_screen == SCREEN_MINES ||
         g_screen == SCREEN_SNAKE || g_screen == SCREEN_2048 || g_screen == SCREEN_TOOLS || g_screen == SCREEN_TIMER || g_screen == SCREEN_STOPWATCH ||
         g_screen == SCREEN_REMOTE || g_screen == SCREEN_CARD || g_screen == SCREEN_ORACLE ||
         g_screen == SCREEN_DICE || g_screen == SCREEN_COIN || g_screen == SCREEN_WAYPOINTS ||
         g_screen == SCREEN_WAYPOINT || g_screen == SCREEN_KNOTS || g_screen == SCREEN_TODO;
}

static String clockFace(int seconds) {
  int h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
  String out;
  if (h > 0) out = String(h) + ":" + (m < 10 ? "0" : "");
  out += String(m) + ":" + (s < 10 ? "0" : "") + String(s);
  return out;
}

static int timerRemaining() {
  if (!g_timerRunning) return g_timerLeftSec;
  int32_t ms = (int32_t)(g_timerEndMs - millis());
  return ms <= 0 ? 0 : (ms + 999) / 1000;
}

static uint32_t stopwatchMs() {
  return g_swAccumMs + (g_swRunning ? millis() - g_swStartMs : 0);
}

static void drawBig(const String& text, int baseline) {
  g_canvas.setTextColor(1);
  g_canvas.setFont(&FreeSansBold24pt7b);
  if (textWidth(text) > EPD_WIDTH - 8) {
    // too wide for the big type: the next size down, doubled, is about two thirds as wide
    g_canvas.setFont(&FreeSansBold9pt7b);
    g_canvas.setTextSize(2);
    if (textWidth(text) > EPD_WIDTH - 8) g_canvas.setTextSize(1);
    drawCentered(text, baseline - 6);
    g_canvas.setTextSize(1);
    return;
  }
  drawCentered(text, baseline);
}

static void drawMedium(const String& text, int baseline) {
  g_canvas.setTextColor(1);
  g_canvas.setFont(&FreeSansBold9pt7b);
  drawCentered(fitText(text, EPD_WIDTH - 8), baseline);
}

static void drawHint(const String& text, int top) {
  g_canvas.setTextColor(1);
  drawSmallCentered(text, top);
}

static String toolsLabel(int i) {
  switch (i) {
    case 0:
      if (g_timerRunning) return "Timer (" + clockFace(timerRemaining()) + " left)";
      return "Timer";
    case 1: return g_swRunning ? "Stopwatch (running)" : "Stopwatch";
    case 2: return g_pomoRunning ? "Pomodoro (running)" : "Pomodoro";
    case 3: return "Tally counter";
    case 4: return "Tip and split";
    case 5: return "Converter";
    case 6: return "Morse code";
    case 7: return g_waypointCount ? "Waypoints (" + String(g_waypointCount) + ")" : String("Waypoints");
    default: return "Games  >";
  }
}

static String gamesLabel(int i) {
  static const char* const NAMES[GAMES_COUNT] = {"Snake", "2048", "Hangman", "Minesweeper",
                                                 "Oracle", "Dice", "Coin flip"};
  return NAMES[i];
}

static bool isGameScreen() {
  return g_screen == SCREEN_SNAKE || g_screen == SCREEN_2048 || g_screen == SCREEN_HANGMAN ||
         g_screen == SCREEN_MINES || g_screen == SCREEN_ORACLE || g_screen == SCREEN_DICE ||
         g_screen == SCREEN_COIN;
}

// ---- Waypoints and the knots shortcut -----------------------------------
static String waypointLabel(int i) {
  if (i == 0) return "Mark this spot";
  const Waypoint& w = g_waypoints[i - 1];
  String r = rangeText(w);
  return r.length() ? String(w.name) + "  " + r : String(w.name);
}

static void drawWaypoints() {
  String age = g_fix.ok ? agoText(g_fix.at) : String("no position");
  drawHeader("WAYPOINTS", age.length() ? "phone: " + age : String("phone position"));
  if (g_wpNote.length()) {
    g_canvas.setTextColor(1);
    drawSmallCentered(g_wpNote, 56);
    drawSmallCentered("Press any button", 76);
    return;
  }
  keepVisible(g_wpSel, g_wpTop);
  drawRows(g_waypointCount + 1, g_wpSel, g_wpTop, waypointLabel);
}

static void drawWaypoint() {
  if (g_wpOpen >= g_waypointCount) { drawWaypoints(); return; }
  const Waypoint& w = g_waypoints[g_wpOpen];
  drawHeader(w.name, "EXIT = back");
  drawMedium(String(w.lat, 5) + ", " + String(w.lon, 5), 44);
  String saved = agoText(w.at);
  drawHint(saved.length() ? "Saved " + saved : String("Saved (clock was not set)"), 54);
  String r = rangeText(w);
  if (r.length()) {
    String age = agoText(g_fix.at);
    drawMedium(r == "here" ? String("You are here") : r, 82);
    drawHint(age.length() ? "from the phone's position " + age : String("from the phone's last position"), 92);
  }
  drawHint(g_wpArmed ? "Press DOWN again to delete it" : "DOWN twice = delete", 110);
}

static int g_knotItems[12];
static int g_knotCount = 0, g_knotSel = 0, g_knotTop = 0;

// The reading library already has several rope chapters; this gathers them.
static void findKnots() {
  g_knotCount = 0;
  for (int i = 0; i < Library::itemCount() && g_knotCount < 12; i++) {
    String t = Library::itemTitle(i);
    t.toLowerCase();
    if (t.indexOf("knot") >= 0 || t.indexOf("lashing") >= 0 || t.indexOf("cordage") >= 0) g_knotItems[g_knotCount++] = i;
  }
  if (g_knotSel >= g_knotCount) g_knotSel = 0;
}

static String knotLabel(int i) {
  return String(Library::itemTitle(g_knotItems[i])) + " (" + String(Library::itemPageCount(g_knotItems[i])) + " pages)";
}

static void drawKnots() {
  drawHeader("KNOTS AND LASHINGS", "EXIT = back");
  if (g_knotCount == 0) {
    g_canvas.setTextColor(1);
    drawSmallCentered("The library has no rope chapters.", 60);
    return;
  }
  keepVisible(g_knotSel, g_knotTop);
  drawRows(g_knotCount, g_knotSel, g_knotTop, knotLabel);
}

static void drawTools() {
  drawHeader("TOOLS", "EXIT = back");
  keepVisible(g_toolsSel, g_toolsTop);
  drawRows(TOOLS_COUNT, g_toolsSel, g_toolsTop, toolsLabel);
}

static void drawGames() {
  drawHeader("GAMES", "EXIT = back");
  drawRows(GAMES_COUNT, g_gamesSel, 0, gamesLabel);
}

static int pomoRemaining() {
  if (!g_pomoRunning) return g_pomoLeftSec;
  int32_t ms = (int32_t)(g_pomoEndMs - millis());
  return ms <= 0 ? 0 : (ms + 999) / 1000;
}

static void drawPomo() {
  drawHeader("POMODORO", String(g_pomoDone) + " done");
  if (g_pomoPhase == 0) {
    drawMedium("25 minutes on, 5 off", 54);
    drawHint("Press the wheel to start", 72);
    return;
  }
  drawMedium(g_pomoPhase == 1 ? "Focus" : "Break", 34);
  int left = pomoRemaining();
  drawBig(left >= 60 ? String((left + 59) / 60) + " min" : String(left) + " sec", 78);
  drawHint(g_pomoRunning ? "Press: pause     MENU: reset" : "Paused.  Press: go on    MENU: reset", 96);
}

static void drawTally() {
  drawHeader("TALLY", "EXIT = back");
  drawBig(String(g_tally), 70);
  drawHint("Press or up: +1    Down: -1    MENU: zero", 96);
}

static String money(float amount) {
  char text[16];
  snprintf(text, sizeof(text), "$%.2f", amount);
  return String(text);
}

static void drawTip() {
  drawHeader("TIP AND SPLIT", "Press = next field");
  g_canvas.setTextColor(1);
  g_canvas.setFont(&FreeSansBold9pt7b);
  const String fields[3] = {"Bill $" + String(g_tipBill), "Tip " + String(g_tipPercent) + "%",
                            String(g_tipPeople) + (g_tipPeople == 1 ? " person" : " people")};
  int x = 6;
  for (int i = 0; i < 3; i++) {
    int w = textWidth(fields[i]);
    if (i == g_tipField) {
      g_canvas.fillRect(x - 3, 20, w + 7, 20, 1);
      g_canvas.setTextColor(0);
    }
    g_canvas.setCursor(x, 35);
    g_canvas.print(fields[i]);
    g_canvas.setTextColor(1);
    x += w + 12;
  }
  float tip = g_tipBill * g_tipPercent / 100.0f;
  float total = g_tipBill + tip;
  g_canvas.setFont(&FreeSans9pt7b);
  g_canvas.setCursor(6, 62);
  g_canvas.print("Tip " + money(tip) + "   Total " + money(total));
  g_canvas.setFont(&FreeSansBold9pt7b);
  g_canvas.setCursor(6, 86);
  g_canvas.print("Each pays " + money(total / g_tipPeople));
  drawHint("Wheel changes the highlighted number", 104);
}

struct ConvPair {
  const char* from;
  const char* to;
  float scale, offset;  // to = from * scale + offset
};
static const ConvPair CONV_PAIRS[] = {
    {"mi", "km", 1.609344f, 0},  {"lb", "kg", 0.45359237f, 0}, {"F", "C", 5.0f / 9.0f, -160.0f / 9.0f},
    {"in", "cm", 2.54f, 0},      {"ft", "m", 0.3048f, 0},      {"oz", "g", 28.349523f, 0},
    {"gal", "L", 3.785412f, 0},  {"cup", "mL", 236.588f, 0},   {"mph", "km/h", 1.609344f, 0},
};
static const int CONV_COUNT = sizeof(CONV_PAIRS) / sizeof(CONV_PAIRS[0]);

static void drawConvert() {
  const ConvPair& pair = CONV_PAIRS[g_convPair];
  drawHeader("CONVERTER", String(g_convPair + 1) + "/" + String(CONV_COUNT));
  float out = g_convReverse ? (g_convValue - pair.offset) / pair.scale
                            : g_convValue * pair.scale + pair.offset;
  char text[24];
  snprintf(text, sizeof(text), fabsf(out) >= 100 ? "%.1f" : "%.2f", out);
  drawMedium(String(g_convValue) + " " + (g_convReverse ? pair.to : pair.from) + "  =", 40);
  drawBig(String(text) + " " + (g_convReverse ? pair.from : pair.to), 82);
  drawHint("Wheel: number   Press: next unit", 96);
  drawHint("MENU: swap direction", 107);
}

static void drawFind() {
  drawHeader("FIND MY PHONE", "EXIT = back");
  if (!g_findSent) {
    drawMedium("Press the wheel", 54);
    drawHint("and your phone will ring", 70);
  } else if (!g_appListening) {
    drawMedium("The app is not connected", 54);
    drawHint("Open Inkfob on the phone, then try again", 70);
  } else {
    drawMedium("Ringing your phone", 54);
    drawHint("Press again to ring once more", 70);
  }
}

static const char* const MORSE_CODES[36] = {
    ".-",   "-...", "-.-.", "-..",  ".",     "..-.",  "--.",   "....",  "..",    ".---",  "-.-",  ".-..",
    "--",   "-.",   "---",  ".--.", "--.-",  ".-.",   "...",   "-",     "..-",   "...-",  ".--",  "-..-",
    "-.--", "--..", "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

static void drawMorse() {
  drawHeader("MORSE CODE", g_morseQuiz ? "quiz" : "chart");
  char symbol = g_morseAt < 26 ? (char)('A' + g_morseAt) : (char)('0' + g_morseAt - 26);
  drawBig(String(symbol), 56);
  if (g_morseShown) {
    String code;
    for (const char* c = MORSE_CODES[g_morseAt]; *c; c++) code += (*c == '.') ? "o " : "=== ";
    code.trim();
    drawMedium(MORSE_CODES[g_morseAt], 78);
    drawHint(code, 86);
  } else {
    drawMedium("?", 78);
  }
  drawHint(g_morseQuiz ? "Press: show / next    MENU: chart" : "Wheel: browse    MENU: quiz me", 104);
}

static void hangStart() {
  g_hangWord = HANG_WORDS[esp_random() % HANG_COUNT];
  g_hangGuessed = 0;
  g_hangWrong = 0;
}

static bool hangSolved() {
  for (const char* c = g_hangWord; *c; c++) {
    if (!(g_hangGuessed & (1u << (*c - 'A')))) return false;
  }
  return true;
}

static void drawHangman() {
  drawHeader("HANGMAN", String(g_hangWins) + " won");
  g_canvas.setTextColor(1);
  if (!g_hangWord) hangStart();
  // gallows
  g_canvas.drawFastHLine(8, 104, 44, 1);
  g_canvas.drawFastVLine(18, 22, 82, 1);
  g_canvas.drawFastHLine(18, 22, 28, 1);
  g_canvas.drawFastVLine(46, 22, 8, 1);
  if (g_hangWrong > 0) g_canvas.drawCircle(46, 37, 7, 1);
  if (g_hangWrong > 1) g_canvas.drawFastVLine(46, 44, 26, 1);
  if (g_hangWrong > 2) g_canvas.drawLine(46, 50, 36, 62, 1);
  if (g_hangWrong > 3) g_canvas.drawLine(46, 50, 56, 62, 1);
  if (g_hangWrong > 4) g_canvas.drawLine(46, 70, 37, 86, 1);
  if (g_hangWrong > 5) g_canvas.drawLine(46, 70, 55, 86, 1);
  bool lost = g_hangWrong >= 6, won = hangSolved();
  String shown;
  for (const char* c = g_hangWord; *c; c++) {
    bool known = lost || (g_hangGuessed & (1u << (*c - 'A')));
    shown += known ? *c : '_';
    shown += ' ';
  }
  shown.trim();
  g_canvas.setFont(&FreeSansBold9pt7b);
  g_canvas.setCursor(70, 44);
  g_canvas.print(shown);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  if (lost || won) {
    g_canvas.setCursor(70, 62);
    g_canvas.print(won ? "You got it!" : "Out of guesses.");
    g_canvas.setCursor(70, 76);
    g_canvas.print("Press: new word");
    return;
  }
  String missed;
  for (int i = 0; i < 26; i++) {
    bool inWord = strchr(g_hangWord, 'A' + i) != nullptr;
    if ((g_hangGuessed & (1u << i)) && !inWord) missed += (char)('A' + i);
  }
  g_canvas.setCursor(70, 58);
  g_canvas.print("Missed: " + missed);
  g_canvas.setFont(&FreeSansBold9pt7b);
  g_canvas.setCursor(70, 84);
  g_canvas.print("Guess:  < " + String((char)('A' + g_hangLetter)) + " >");
  drawHint("Wheel: letter   Press: guess", 104);
}

static void minesStart(int safeX, int safeY) {
  memset(g_msCell, 0, sizeof(g_msCell));
  int placed = 0;
  while (placed < MS_MINES) {
    int x = (int)(esp_random() % MS_COLS), y = (int)(esp_random() % MS_ROWS);
    if (abs(x - safeX) <= 1 && abs(y - safeY) <= 1) continue;  // the first square is always clear
    if (g_msCell[y * MS_COLS + x] & 0x80) continue;
    g_msCell[y * MS_COLS + x] |= 0x80;
    placed++;
  }
  for (int y = 0; y < MS_ROWS; y++) {
    for (int x = 0; x < MS_COLS; x++) {
      int n = 0;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx >= 0 && ny >= 0 && nx < MS_COLS && ny < MS_ROWS && (g_msCell[ny * MS_COLS + nx] & 0x80)) n++;
        }
      }
      g_msCell[y * MS_COLS + x] |= (uint8_t)(n & 0x0F);
    }
  }
  g_msState = 1;
}

static void minesOpen(int startX, int startY) {
  static uint8_t stack[MS_COLS * MS_ROWS];
  int depth = 0;
  stack[depth++] = (uint8_t)(startY * MS_COLS + startX);
  while (depth > 0) {
    int at = stack[--depth];
    if (g_msCell[at] & 0x40) continue;
    g_msCell[at] |= 0x40;
    if ((g_msCell[at] & 0x8F) != 0) continue;  // only empty squares spread
    int x = at % MS_COLS, y = at / MS_COLS;
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= MS_COLS || ny >= MS_ROWS) continue;
        int next = ny * MS_COLS + nx;
        if (!(g_msCell[next] & 0x40) && depth < (int)sizeof(stack)) stack[depth++] = (uint8_t)next;
      }
    }
  }
}

static void drawMines() {
  int closed = 0;
  for (int i = 0; i < MS_COLS * MS_ROWS; i++) closed += (g_msCell[i] & 0x40) ? 0 : 1;
  String right = g_msState == 2 ? "Boom! Press: again" : g_msState == 3 ? "Cleared! Press: again"
               : String(g_msAcross ? "wheel: across" : "wheel: up/down") + "  MENU: turn";
  drawHeader("MINES", right);
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  const int ox = (EPD_WIDTH - MS_COLS * MS_CELL) / 2, oy = HEADER_H + 3;
  for (int y = 0; y < MS_ROWS; y++) {
    for (int x = 0; x < MS_COLS; x++) {
      uint8_t cell = g_msCell[y * MS_COLS + x];
      int px = ox + x * MS_CELL, py = oy + y * MS_CELL;
      bool open = (cell & 0x40) || (g_msState >= 2 && (cell & 0x80));
      if (!open) {
        g_canvas.fillRect(px + 1, py + 1, MS_CELL - 2, MS_CELL - 2, 1);
      } else {
        g_canvas.drawRect(px + 1, py + 1, MS_CELL - 2, MS_CELL - 2, 1);
        g_canvas.setTextColor(1);
        g_canvas.setCursor(px + 6, py + 5);
        if (cell & 0x80) g_canvas.print("*");
        else if (cell & 0x0F) g_canvas.print(String(cell & 0x0F));
      }
      if (x == g_msX && y == g_msY && g_msState < 2) {
        g_canvas.drawRect(px + 3, py + 3, MS_CELL - 6, MS_CELL - 6, open ? 1 : 0);
        g_canvas.drawRect(px + 4, py + 4, MS_CELL - 8, MS_CELL - 8, open ? 1 : 0);
      }
    }
  }
}

static void snakeFood() {
  for (int tries = 0; tries < 400; tries++) {
    int x = (int)(esp_random() % SN_COLS), y = (int)(esp_random() % SN_ROWS);
    bool onSnake = false;
    for (int i = 0; i < g_snLen; i++) onSnake |= (g_snX[i] == x && g_snY[i] == y);
    if (!onSnake) {
      g_snFoodX = x;
      g_snFoodY = y;
      return;
    }
  }
}

static void snakeStart() {
  g_snLen = 4;
  for (int i = 0; i < g_snLen; i++) {
    g_snX[i] = 8 - i;
    g_snY[i] = SN_ROWS / 2;
  }
  g_snDir = 0;
  g_snTurn = 0;
  g_snAlive = true;
  g_snPaused = false;
  g_snNextMs = millis() + SN_STEP_MS;
  snakeFood();
}

static void snakeStep() {
  g_snDir = (g_snDir + g_snTurn + 4) % 4;
  g_snTurn = 0;
  static const int DX[] = {1, 0, -1, 0}, DY[] = {0, 1, 0, -1};
  int x = g_snX[0] + DX[g_snDir], y = g_snY[0] + DY[g_snDir];
  bool hit = (x < 0 || y < 0 || x >= SN_COLS || y >= SN_ROWS);
  for (int i = 0; i < g_snLen - 1 && !hit; i++) hit = (g_snX[i] == x && g_snY[i] == y);
  if (hit) {
    g_snAlive = false;
    if (g_snLen - 4 > g_snBest) {
      g_snBest = g_snLen - 4;
      g_prefs.putInt("snBest", g_snBest);
    }
    return;
  }
  bool ate = (x == g_snFoodX && y == g_snFoodY);
  if (ate && g_snLen < SN_MAX) g_snLen++;
  for (int i = g_snLen - 1; i > 0; i--) {
    g_snX[i] = g_snX[i - 1];
    g_snY[i] = g_snY[i - 1];
  }
  g_snX[0] = x;
  g_snY[0] = y;
  if (ate) snakeFood();
}

static void drawSnake() {
  drawHeader("SNAKE  " + String(g_snLen > 4 ? g_snLen - 4 : 0), "best " + String(g_snBest));
  g_canvas.setTextColor(1);
  if (g_snLen == 0) {
    drawMedium("Snake", 46);
    drawHint("Wheel up: turn left", 60);
    drawHint("Wheel down: turn right", 70);
    drawHint("Press the wheel to start or pause", 86);
    return;
  }
  const int ox = 1, oy = HEADER_H + 2;
  for (int i = 0; i < g_snLen; i++) {
    int px = ox + g_snX[i] * SN_CELL, py = oy + g_snY[i] * SN_CELL;
    if (i == 0) g_canvas.fillRect(px, py, SN_CELL, SN_CELL, 1);
    else g_canvas.fillRect(px + 1, py + 1, SN_CELL - 2, SN_CELL - 2, 1);
  }
  g_canvas.drawCircle(ox + g_snFoodX * SN_CELL + 3, oy + g_snFoodY * SN_CELL + 3, 3, 1);
  if (!g_snAlive || g_snPaused) {
    String text = g_snAlive ? "Paused" : "Game over: " + String(g_snLen - 4);
    g_canvas.setFont(&FreeSansBold9pt7b);
    int w = textWidth(text) + 20;
    g_canvas.fillRect((EPD_WIDTH - w) / 2, 46, w, 40, 0);
    g_canvas.drawRect((EPD_WIDTH - w) / 2, 46, w, 40, 1);
    drawCentered(text, 64);
    drawSmallCentered(g_snAlive ? "Press: go on" : "Press: play again", 72);
  }
}

static void tilesAdd() {
  int empty[16], n = 0;
  for (int i = 0; i < 16; i++) if (!g_tiles[i]) empty[n++] = i;
  if (n) g_tiles[empty[esp_random() % n]] = (esp_random() % 10 == 0) ? 2 : 1;
}

static void tilesStart() {
  memset(g_tiles, 0, sizeof(g_tiles));
  g_tScore = 0;
  g_tOver = false;
  g_tStarted = true;
  tilesAdd();
  tilesAdd();
}

// Slides the board one way: 0 left, 1 right, 2 up, 3 down. Returns true if anything moved.
static bool tilesMove(int way) {
  bool moved = false;
  for (int line = 0; line < 4; line++) {
    int idx[4];
    for (int k = 0; k < 4; k++) {
      int pos = (way == 0 || way == 2) ? k : 3 - k;
      idx[k] = (way < 2) ? line * 4 + pos : pos * 4 + line;
    }
    uint8_t out[4] = {0, 0, 0, 0};
    int n = 0;
    bool merged = false;
    for (int k = 0; k < 4; k++) {
      uint8_t v = g_tiles[idx[k]];
      if (!v) continue;
      if (n > 0 && out[n - 1] == v && !merged) {
        out[n - 1]++;
        g_tScore += 1u << out[n - 1];
        merged = true;
      } else {
        out[n++] = v;
        merged = false;
      }
    }
    for (int k = 0; k < 4; k++) {
      if (g_tiles[idx[k]] != out[k]) moved = true;
      g_tiles[idx[k]] = out[k];
    }
  }
  return moved;
}

static bool tilesStuck() {
  for (int i = 0; i < 16; i++) {
    if (!g_tiles[i]) return false;
    if (i % 4 < 3 && g_tiles[i] == g_tiles[i + 1]) return false;
    if (i < 12 && g_tiles[i] == g_tiles[i + 4]) return false;
  }
  return true;
}

static void draw2048() {
  drawHeader("2048", "EXIT = back");
  g_canvas.setTextColor(1);
  if (!g_tStarted) tilesStart();
  const int ox = 3, oy = HEADER_H + 1, pitch = 27;
  for (int i = 0; i < 16; i++) {
    int x = ox + (i % 4) * pitch, y = oy + (i / 4) * pitch;
    uint8_t v = g_tiles[i];
    bool dark = v >= 7;  // 128 and up are drawn white on black
    if (dark) g_canvas.fillRoundRect(x, y, pitch - 2, pitch - 2, 3, 1);
    else g_canvas.drawRoundRect(x, y, pitch - 2, pitch - 2, 3, 1);
    if (!v) continue;
    String text = String(1u << v);
    g_canvas.setTextColor(dark ? 0 : 1);
    if (text.length() <= 2) {
      g_canvas.setFont(&FreeSansBold9pt7b);
      g_canvas.setCursor(x + (pitch - 2 - textWidth(text)) / 2 - 1, y + 18);
    } else {
      g_canvas.setFont(nullptr);
      g_canvas.setTextSize(1);
      g_canvas.setCursor(x + (pitch - 2 - (int)text.length() * 6) / 2 + 1, y + 9);
    }
    g_canvas.print(text);
  }
  g_canvas.setTextColor(1);
  const int px = 118;
  g_canvas.setFont(&FreeSansBold9pt7b);
  g_canvas.setCursor(px, 34);
  g_canvas.print(g_tOver ? "Game over" : "Score");
  g_canvas.setCursor(px, 54);
  g_canvas.print(String(g_tScore));
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setCursor(px, 64);
  g_canvas.print("Best " + String(g_tBest));
  g_canvas.setCursor(px, 82);
  g_canvas.print(g_tOver ? "Press: new game" : "Wheel: up / down");
  g_canvas.setCursor(px, 94);
  g_canvas.print(g_tOver ? "" : "MENU: left");
  g_canvas.setCursor(px, 106);
  g_canvas.print(g_tOver ? "" : "Press: right");
}

static void drawTimer() {
  drawHeader("TIMER", g_timerRunning ? "running" : "EXIT = back");
  if (g_timerDone) {
    drawBig("Time's up", 64);
    drawHint(clockFace(g_timerSetSec) + " timer finished", 82);
    drawHint("Press any button", 100);
    return;
  }
  drawBig(clockFace(timerRemaining()), 64);
  if (g_timerRunning) {
    drawHint("Press: pause     MENU: reset", 86);
    drawHint("Keeps counting if you leave this screen", 102);
  } else if (g_timerLeftSec != g_timerSetSec) {
    drawHint("Paused", 82);
    drawHint("Press: resume     MENU: reset", 98);
  } else {
    drawHint("Up / Down: set the time", 86);
    drawHint("Press: start", 102);
  }
}

static void drawStopwatch() {
  drawHeader("STOPWATCH", g_swRunning ? "running" : "EXIT = back");
  uint32_t ms = stopwatchMs();
  String text = clockFace((int)(ms / 1000));
  if (!g_swRunning) text += "." + String((int)((ms % 1000) / 100));
  drawBig(text, 64);
  if (g_swRunning) {
    drawHint("Press: stop", 92);
  } else if (ms == 0) {
    drawHint("Press: start", 92);
  } else {
    drawHint("Press: resume     MENU: reset", 92);
  }
}

static void drawRemote() {
  drawHeader("CAMERA REMOTE", linkStatus());
  drawMedium("Press = take photo", 38);
  drawHint("Open the Camera app on the iPhone first.", 52);
  drawHint("Up / Down: volume", 72);
  drawHint("MENU: play / pause", 86);
  drawHint(g_secured ? "The light blinks when a press is sent" : "Phone not connected", 104);
}

// The card: a QR code on the left, name and details on the right.
// asleep = true adds the sleep notes underneath.
static void drawCard(bool asleep) {
  static uint8_t code[256];
  static bool ok = false;
  if (g_cardQrStale) {
    static uint8_t scratch[256];
    g_cardQrStale = false;
    ok = g_cardQr.length() && qrcodegen_encodeText(g_cardQr.c_str(), scratch, code, 1, 1, 6, -1, true);  // up to 41 x 41
  }
  g_canvas.fillScreen(0);
  g_canvas.setTextColor(1);

  int textX = 10;
  if (ok) {
    int size = qrcodegen_getSize(code);
    int scale = EPD_HEIGHT / (size + 4);  // leaves the clear border a scanner needs
    if (scale < 1) scale = 1;
    int px = size * scale;
    int x0 = 2 * scale + 2;
    int y0 = (EPD_HEIGHT - px) / 2;
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        if (qrcodegen_getModule(code, x, y)) g_canvas.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, 1);
      }
    }
    textX = x0 + px + 2 * scale + 6;
  }

  int room = EPD_WIDTH - textX - 2;
  String first = g_cardName, second;
  g_canvas.setFont(&FreeSansBold9pt7b);
  int space = first.indexOf(' ');
  if (textWidth(first) > room && space > 0) {
    second = first.substring(space + 1);
    first = first.substring(0, space);
  }
  int y = 26;
  g_canvas.setCursor(textX, y);
  g_canvas.print(first);
  if (second.length()) {
    y += 18;
    g_canvas.setCursor(textX, y);
    g_canvas.print(second);
  }
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  int maxChars = room / 6;
  y += 10;
  for (const String& line : {g_cardLine1, g_cardLine2}) {
    if (!line.length()) continue;
    g_canvas.setCursor(textX, y);
    g_canvas.print(line.substring(0, maxChars));
    y += 11;
  }
  if (asleep) {
    int unread = unreadCount();
    String when = clockText();
    if (unread > 0) {
      g_canvas.setCursor(textX, EPD_HEIGHT - 34);
      g_canvas.print((String(unread) + " new").substring(0, maxChars));
    }
    g_canvas.setCursor(textX, EPD_HEIGHT - 22);
    g_canvas.print((when.length() ? "Asleep " + when : String("Asleep")).substring(0, maxChars));
    g_canvas.setCursor(textX, EPD_HEIGHT - 11);
    g_canvas.print(String("Any button wakes").substring(0, maxChars));
  }
}

static void drawOracle() {
  drawHeader("ORACLE", "EXIT = back");
  if (g_oracleAnswer < 0) {
    drawMedium("Ask a yes or no question", 54);
    drawHint("then press the wheel", 70);
    return;
  }
  drawMedium(ORACLE_ANSWERS[g_oracleAnswer], 62);
  drawHint("Answer " + String(g_oracleCount) + "     Press: ask again", 96);
}

static String diceName(int type) {
  return DICE_SIDES[type] ? "d" + String(DICE_SIDES[type]) : String("2 x d6");
}

static void drawDice() {
  drawHeader("DICE", diceName(g_diceType));
  if (g_diceA == 0) {
    drawBig(diceName(g_diceType), 64);
    drawHint("Press: roll     Up / Down: change dice", 92);
    return;
  }
  if (DICE_SIDES[g_diceType]) {
    drawBig(String(g_diceA), 64);
  } else {
    drawBig(String(g_diceA) + " + " + String(g_diceB) + " = " + String(g_diceA + g_diceB), 64);
  }
  drawHint("Roll " + String(g_diceCount) + "     Up / Down: change dice", 92);
}

static void drawCoin() {
  drawHeader("COIN FLIP", "EXIT = back");
  if (g_coinSide < 0) {
    drawMedium("Press the wheel to flip", 62);
    return;
  }
  drawBig(g_coinSide ? "HEADS" : "TAILS", 64);
  drawHint("Flip " + String(g_coinCount) + "     Heads " + String(g_coinHeads) + "   Tails " +
           String(g_coinCount - g_coinHeads), 92);
}

// Sends one media key to the phone, the way a Bluetooth camera clicker does.
// key: 0 next, 1 previous, 2 play/pause, 3 volume up, 4 volume down.
static bool sendHidKey(int key) {
  if (!g_hidInput || !g_secured) return false;
  uint8_t report = (uint8_t)(1 << key);
  g_hidInput->setValue(&report, 1);
  g_hidInput->notify();
  delay(40);
  report = 0;
  g_hidInput->setValue(&report, 1);
  g_hidInput->notify();
  return true;
}

static void resetTimer() {
  g_timerRunning = false;
  g_timerDone = false;
  g_timerLeftSec = g_timerSetSec;
}

// Handles a button on any of the tool screens. Returns false when the screen
// does not need redrawing.
static bool onToolPress(int pin) {
  uint32_t at = g_pressAtMs;
  if (g_screen == SCREEN_TOOLS) {
    if (pin == PIN_UP && g_toolsSel > 0) g_toolsSel--;
    if (pin == PIN_DOWN && g_toolsSel < TOOLS_COUNT - 1) g_toolsSel++;
    if (pin == PIN_EXIT) {
      openMainMenu();
      g_menuSel = 5;
    }
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_CLICK) {
      static const Screen TARGETS[TOOLS_COUNT] = {
          SCREEN_TIMER, SCREEN_STOPWATCH, SCREEN_POMO,      SCREEN_TALLY, SCREEN_TIP,
          SCREEN_CONVERT, SCREEN_MORSE,   SCREEN_WAYPOINTS, SCREEN_GAMES};
      g_screen = TARGETS[g_toolsSel];
      g_backTo = SCREEN_TOOLS;
      if (g_screen == SCREEN_WAYPOINTS) { g_wpSel = 0; g_wpNote = ""; }
    }
    return true;
  }
  if (g_screen == SCREEN_WAYPOINTS) {
    int rows = g_waypointCount + 1;
    g_wpNote = "";
    if (pin == PIN_UP && g_wpSel > 0) g_wpSel--;
    if (pin == PIN_DOWN && g_wpSel < rows - 1) g_wpSel++;
    if (pin == PIN_EXIT) g_screen = g_backTo;
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_CLICK) {
      if (g_wpSel == 0) {
        if (!g_fix.ok) g_wpNote = "No position yet. Open the phone app once.";
        else if (g_waypointCount >= WAYPOINT_MAX) g_wpNote = "Full. Delete one first.";
        else if (addWaypoint("")) g_wpSel = g_waypointCount;
      } else {
        g_wpOpen = g_wpSel - 1;
        g_wpArmed = false;
        g_screen = SCREEN_WAYPOINT;
      }
    }
    return true;
  }
  if (g_screen == SCREEN_WAYPOINT) {
    if (pin == PIN_DOWN) {
      if (g_wpArmed) {
        deleteWaypoint(g_wpOpen);
        g_wpSel = min(g_wpSel, g_waypointCount);
        g_screen = SCREEN_WAYPOINTS;
      }
      g_wpArmed = !g_wpArmed && g_screen == SCREEN_WAYPOINT;
    } else {
      g_wpArmed = false;
      g_screen = (pin == PIN_MENU) ? SCREEN_HOME : SCREEN_WAYPOINTS;
    }
    return true;
  }
  if (g_screen == SCREEN_TODO) {
    if (pin == PIN_EXIT) g_screen = g_backTo;
    else if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    else todoPress(pin);
    return true;
  }
  if (g_screen == SCREEN_KNOTS) {
    if (pin == PIN_UP && g_knotSel > 0) g_knotSel--;
    if (pin == PIN_DOWN && g_knotSel < g_knotCount - 1) g_knotSel++;
    if (pin == PIN_EXIT) g_screen = g_backTo;
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_CLICK && g_knotCount > 0) {
      openReader(g_knotItems[g_knotSel], 0);
      g_screen = SCREEN_READER;
    }
    return true;
  }
  if (g_screen == SCREEN_GAMES) {
    if (pin == PIN_UP && g_gamesSel > 0) g_gamesSel--;
    if (pin == PIN_DOWN && g_gamesSel < GAMES_COUNT - 1) g_gamesSel++;
    if (pin == PIN_EXIT) g_screen = SCREEN_TOOLS;
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_CLICK) {
      static const Screen TARGETS[GAMES_COUNT] = {SCREEN_SNAKE,  SCREEN_2048, SCREEN_HANGMAN,
                                                  SCREEN_MINES,  SCREEN_ORACLE, SCREEN_DICE,
                                                  SCREEN_COIN};
      g_screen = TARGETS[g_gamesSel];
    }
    return true;
  }
  if (pin == PIN_EXIT && !(g_screen == SCREEN_TIMER && g_timerDone)) {
    if (g_screen == SCREEN_SNAKE && g_snAlive) g_snPaused = true;
    if (g_screen == SCREEN_TALLY) g_prefs.putInt("tally", g_tally);
    g_screen = isGameScreen() ? SCREEN_GAMES : g_backTo;
    g_forceFull = true;
    return true;
  }

  if (g_screen == SCREEN_POMO) {
    if (pin == PIN_CLICK) {
      if (g_pomoPhase == 0) {
        g_pomoPhase = 1;
        g_pomoLeftSec = 25 * 60;
      }
      if (g_pomoRunning) {
        g_pomoLeftSec = pomoRemaining();
        g_pomoRunning = false;
      } else {
        g_pomoEndMs = millis() + (uint32_t)g_pomoLeftSec * 1000;
        g_pomoRunning = true;
      }
    }
    if (pin == PIN_MENU) {
      g_pomoPhase = 0;
      g_pomoRunning = false;
      g_pomoLeftSec = 25 * 60;
    }
    return true;
  }
  if (g_screen == SCREEN_TALLY) {
    if (pin == PIN_CLICK || pin == PIN_UP) g_tally++;
    if (pin == PIN_DOWN && g_tally > 0) g_tally--;
    if (pin == PIN_MENU) g_tally = 0;
    return true;
  }
  if (g_screen == SCREEN_TIP) {
    int step = (pin == PIN_UP) ? 1 : (pin == PIN_DOWN) ? -1 : 0;
    if (pin == PIN_CLICK) g_tipField = (g_tipField + 1) % 3;
    if (g_tipField == 0) g_tipBill = constrain(g_tipBill + step, 1, 999);
    if (g_tipField == 1) g_tipPercent = constrain(g_tipPercent + step, 0, 40);
    if (g_tipField == 2) g_tipPeople = constrain(g_tipPeople + step, 1, 20);
    return pin != PIN_MENU;
  }
  if (g_screen == SCREEN_CONVERT) {
    if (pin == PIN_UP) g_convValue = min(g_convValue + 1, 9999);
    if (pin == PIN_DOWN) g_convValue = max(g_convValue - 1, -99);
    if (pin == PIN_CLICK) g_convPair = (g_convPair + 1) % CONV_COUNT;
    if (pin == PIN_MENU) g_convReverse = !g_convReverse;
    return true;
  }
  if (g_screen == SCREEN_FIND) {
    if (pin == PIN_CLICK) {
      g_findSent = true;
      if (g_appListening) appNotifyText('B', "");
    }
    return pin == PIN_CLICK;
  }
  if (g_screen == SCREEN_MORSE) {
    if (pin == PIN_MENU) {
      g_morseQuiz = !g_morseQuiz;
      g_morseShown = !g_morseQuiz;
      if (g_morseQuiz) g_morseAt = (int)(esp_random() % 36);
    } else if (g_morseQuiz) {
      if (pin == PIN_CLICK) {
        if (g_morseShown) g_morseAt = (int)(esp_random() % 36);
        g_morseShown = !g_morseShown;
      }
    } else {
      if (pin == PIN_DOWN || pin == PIN_CLICK) g_morseAt = (g_morseAt + 1) % 36;
      if (pin == PIN_UP) g_morseAt = (g_morseAt + 35) % 36;
    }
    return true;
  }
  if (g_screen == SCREEN_HANGMAN) {
    if (!g_hangWord) hangStart();
    bool over = g_hangWrong >= 6 || hangSolved();
    if (over) {
      if (pin == PIN_CLICK) hangStart();
      return pin == PIN_CLICK;
    }
    if (pin == PIN_UP || pin == PIN_DOWN) {
      // skip letters that have been tried already
      for (int tries = 0; tries < 26; tries++) {
        g_hangLetter = (g_hangLetter + (pin == PIN_DOWN ? 1 : 25)) % 26;
        if (!(g_hangGuessed & (1u << g_hangLetter))) break;
      }
    }
    if (pin == PIN_CLICK && !(g_hangGuessed & (1u << g_hangLetter))) {
      g_hangGuessed |= (1u << g_hangLetter);
      if (!strchr(g_hangWord, 'A' + g_hangLetter)) g_hangWrong++;
      else if (hangSolved()) g_hangWins++;
    }
    return pin != PIN_MENU;
  }
  if (g_screen == SCREEN_MINES) {
    if (pin == PIN_MENU) {
      g_msAcross = !g_msAcross;
      return true;
    }
    if (g_msState >= 2) {
      if (pin == PIN_CLICK) {
        memset(g_msCell, 0, sizeof(g_msCell));
        g_msState = 0;
      }
      return pin == PIN_CLICK;
    }
    int step = (pin == PIN_DOWN) ? 1 : (pin == PIN_UP) ? -1 : 0;
    if (g_msAcross) g_msX = (g_msX + step + MS_COLS) % MS_COLS;
    else g_msY = (g_msY + step + MS_ROWS) % MS_ROWS;
    if (pin == PIN_CLICK) {
      if (g_msState == 0) minesStart(g_msX, g_msY);
      if (g_msCell[g_msY * MS_COLS + g_msX] & 0x80) {
        g_msState = 2;
      } else {
        minesOpen(g_msX, g_msY);
        int closed = 0;
        for (int i = 0; i < MS_COLS * MS_ROWS; i++) closed += (g_msCell[i] & 0x40) ? 0 : 1;
        if (closed == MS_MINES) g_msState = 3;
      }
    }
    return true;
  }

  if (g_screen == SCREEN_SNAKE) {
    if (pin == PIN_CLICK) {
      if (!g_snAlive) snakeStart();
      else {
        g_snPaused = !g_snPaused;
        g_snNextMs = millis() + SN_STEP_MS;
      }
      return true;
    }
    if (pin == PIN_MENU) return false;
    if (g_snAlive && !g_snPaused) g_snTurn = (pin == PIN_UP) ? -1 : 1;
    return false;  // the next step draws it
  }
  if (g_screen == SCREEN_2048) {
    if (g_tOver) {
      if (pin == PIN_CLICK) tilesStart();
      return pin == PIN_CLICK;
    }
    int way = (pin == PIN_MENU) ? 0 : (pin == PIN_CLICK) ? 1 : (pin == PIN_UP) ? 2 : 3;
    if (!tilesMove(way)) return false;
    tilesAdd();
    if (g_tScore > g_tBest) g_tBest = g_tScore;
    if (tilesStuck()) {
      g_tOver = true;
      g_prefs.putUInt("t48Best", g_tBest);
    }
    return true;
  }

  if (g_screen == SCREEN_TIMER) {
    if (g_timerDone) {
      resetTimer();
    } else if (g_timerRunning) {
      if (pin == PIN_CLICK) {
        g_timerLeftSec = timerRemaining();
        g_timerRunning = false;
      }
      if (pin == PIN_MENU) resetTimer();
    } else {
      bool fresh = (g_timerLeftSec == g_timerSetSec);
      if (fresh && (pin == PIN_UP || pin == PIN_DOWN)) {
        int minutes = g_timerSetSec / 60;
        if (pin == PIN_UP) minutes += (minutes < 10) ? 1 : (minutes < 60) ? 5 : 15;
        else minutes -= (minutes <= 10) ? 1 : (minutes <= 60) ? 5 : 15;
        if (minutes < 1) minutes = 1;
        if (minutes > 180) minutes = 180;
        g_timerSetSec = g_timerLeftSec = minutes * 60;
      }
      if (pin == PIN_CLICK && g_timerLeftSec > 0) {
        if (fresh) g_prefs.putInt("timerSec", g_timerSetSec);
        g_timerEndMs = at + (uint32_t)g_timerLeftSec * 1000UL;
        g_timerRunning = true;
        g_timerShownKey = -1;
      }
      if (pin == PIN_MENU) resetTimer();
    }
  } else if (g_screen == SCREEN_STOPWATCH) {
    if (pin == PIN_CLICK) {
      if (g_swRunning) {
        g_swAccumMs += at - g_swStartMs;
        g_swRunning = false;
      } else {
        g_swStartMs = at;
        g_swRunning = true;
        g_swShownSec = -1;
      }
    }
    if (pin == PIN_MENU && !g_swRunning) g_swAccumMs = 0;
  } else if (g_screen == SCREEN_REMOTE) {
    int key = (pin == PIN_CLICK || pin == PIN_UP) ? 3 : (pin == PIN_DOWN) ? 4 : 2;
    if (sendHidKey(key)) blinkLed(1);
    return false;  // nothing on the screen changes
  } else if (g_screen == SCREEN_CARD) {
    g_screen = g_backTo;
  } else if (g_screen == SCREEN_ORACLE) {
    if (pin == PIN_CLICK) {
      g_oracleAnswer = (int)(esp_random() % ORACLE_COUNT);
      g_oracleCount++;
    }
  } else if (g_screen == SCREEN_DICE) {
    if (pin == PIN_UP || pin == PIN_DOWN) {
      g_diceType = (g_diceType + (pin == PIN_DOWN ? 1 : DICE_TYPES - 1)) % DICE_TYPES;
      g_diceA = g_diceB = 0;
    }
    if (pin == PIN_CLICK) {
      int sides = DICE_SIDES[g_diceType] ? DICE_SIDES[g_diceType] : 6;
      g_diceA = 1 + (int)(esp_random() % sides);
      g_diceB = 1 + (int)(esp_random() % sides);
      g_diceCount++;
    }
  } else if (g_screen == SCREEN_COIN) {
    if (pin == PIN_CLICK) {
      g_coinSide = (int)(esp_random() & 1);
      g_coinCount++;
      if (g_coinSide) g_coinHeads++;
    }
  }
  return true;
}

// Keeps the timer and stopwatch ticking; called from the main loop.
static void serviceTools() {
  if (g_timerRunning && timerRemaining() == 0) {
    g_timerRunning = false;
    g_timerLeftSec = g_timerSetSec;
    g_timerDone = true;
    g_timerDoneAtMs = millis();
    g_timerBlinking = true;
    g_lastActivityMs = millis();
    g_screen = SCREEN_TIMER;
    markDirty(0);
  }
  if (g_timerBlinking) {
    uint32_t since = millis() - g_timerDoneAtMs;
    if (!g_timerDone || since > 20000) {
      g_timerBlinking = false;
      digitalWrite(PIN_LED, LOW);
    } else {
      digitalWrite(PIN_LED, (since / 150) % 2 ? LOW : HIGH);
    }
  }
  // The screen is slow, so count in 5-second steps until the last minute.
  if (g_screen == SCREEN_TIMER && g_timerRunning) {
    int left = timerRemaining();
    int key = (left > 60) ? 100000 + (left + 4) / 5 : left;
    if (key != g_timerShownKey) {
      g_timerShownKey = key;
      markDirty(1);
    }
  }
  if (g_pomoRunning && pomoRemaining() == 0) {
    // one phase ends and the other begins
    if (g_pomoPhase == 1) g_pomoDone++;
    g_pomoPhase = (g_pomoPhase == 1) ? 2 : 1;
    g_pomoLeftSec = (g_pomoPhase == 1) ? 25 * 60 : 5 * 60;
    g_pomoEndMs = millis() + (uint32_t)g_pomoLeftSec * 1000;
    g_timerDoneAtMs = millis();
    g_timerBlinking = true;
    g_timerDone = true;   // borrows the timer's light: it blinks for a few seconds
    g_pomoBlink = true;
    g_lastActivityMs = millis();
    g_screen = SCREEN_POMO;
    g_forceFull = true;
    markDirty(0);
  }
  if (g_pomoBlink && millis() - g_timerDoneAtMs > 6000) {
    g_pomoBlink = false;
    g_timerDone = false;
  }
  if (g_screen == SCREEN_POMO && g_pomoRunning) {
    int left = pomoRemaining();
    int key = (left > 60) ? 100000 + (left + 59) / 60 : left / 5;
    if (key != g_pomoShownKey) {
      g_pomoShownKey = key;
      markDirty(1);
    }
  }
  if (g_screen == SCREEN_SNAKE && g_snAlive && !g_snPaused &&
      (int32_t)(millis() - g_snNextMs) >= 0) {
    g_snNextMs = millis() + SN_STEP_MS;
    snakeStep();
    g_lastActivityMs = millis();
    markDirty(1);
  }
  if (g_screen == SCREEN_STOPWATCH && g_swRunning) {
    int sec = (int)(stopwatchMs() / 1000);
    if (sec != g_swShownSec) {
      g_swShownSec = sec;
      markDirty(1);
    }
  }
}

static void render() {
  g_canvas.fillScreen(0);
  switch (g_screen) {
    case SCREEN_HOME: drawHome(); break;
    case SCREEN_MENU: drawMainMenu(); break;
    case SCREEN_LIBRARY: drawLibrary(); break;
    case SCREEN_READER: drawReader(); break;
    case SCREEN_MEDIA: drawMedia(); break;
    case SCREEN_DETAIL: drawDetail(); break;
    case SCREEN_SETTINGS: drawSettings(); break;
    case SCREEN_STATUS: drawStatus(); break;
    case SCREEN_TOOLS: drawTools(); break;
    case SCREEN_TIMER: drawTimer(); break;
    case SCREEN_STOPWATCH: drawStopwatch(); break;
    case SCREEN_REMOTE: drawRemote(); break;
    case SCREEN_CARD: drawCard(false); break;
    case SCREEN_ORACLE: drawOracle(); break;
    case SCREEN_DICE: drawDice(); break;
    case SCREEN_COIN: drawCoin(); break;
    case SCREEN_FILES: drawFiles(); break;
    case SCREEN_NOTE: drawNote(); break;
    case SCREEN_UPDATE: drawUpdate(); break;
    case SCREEN_TODAY: drawToday(); break;
    case SCREEN_DECK: drawDeck(); break;
    case SCREEN_GAMES: drawGames(); break;
    case SCREEN_POMO: drawPomo(); break;
    case SCREEN_TALLY: drawTally(); break;
    case SCREEN_TIP: drawTip(); break;
    case SCREEN_CONVERT: drawConvert(); break;
    case SCREEN_FIND: drawFind(); break;
    case SCREEN_WAYPOINTS: drawWaypoints(); break;
    case SCREEN_WAYPOINT: drawWaypoint(); break;
    case SCREEN_KNOTS: drawKnots(); break;
    case SCREEN_TODO: drawTodo(false); break;
    case SCREEN_GROUP: drawGroup(); break;
    case SCREEN_MORSE: drawMorse(); break;
    case SCREEN_HANGMAN: drawHangman(); break;
    case SCREEN_MINES: drawMines(); break;
    case SCREEN_SNAKE: drawSnake(); break;
    case SCREEN_2048: draw2048(); break;
    default: drawList(); break;
  }
  // A ticking timer would otherwise flash every half minute.
  bool ticking = (g_screen == SCREEN_TIMER || g_screen == SCREEN_STOPWATCH ||
                  g_screen == SCREEN_SNAKE || g_screen == SCREEN_2048 ||
                  g_screen == SCREEN_MINES || g_screen == SCREEN_HANGMAN ||
                  g_screen == SCREEN_TIP || g_screen == SCREEN_CONVERT || g_screen == SCREEN_TALLY);
  bool full = g_forceFull || !g_fastRefresh || g_fastCount >= (ticking ? 150 : 30);
  Epd::show(g_canvas.getBuffer(), full);
  g_fastCount = full ? 0 : g_fastCount + 1;
  g_forceFull = false;
}

// ---------------------------------------------------------------- buttons

static void enterSleep(bool drawFace);

static const int BUTTON_PINS[] = {PIN_UP, PIN_DOWN, PIN_CLICK, PIN_EXIT, PIN_MENU};
static const int BUTTON_COUNT = 5;

// Watches the buttons every few milliseconds, even while the screen is busy
// redrawing, and queues each press for the main loop.
static void inputTask(void*) {
  bool stable[BUTTON_COUNT];
  bool last[BUTTON_COUNT];
  uint32_t since[BUTTON_COUNT];
  for (int i = 0; i < BUTTON_COUNT; i++) {
    stable[i] = last[i] = (digitalRead(BUTTON_PINS[i]) == LOW);  // ignore anything held at boot
    since[i] = millis();
  }
  int heldBackPin = -1;  // an up/down nudge waiting to see if it is really a press
  uint32_t heldBackUntil = 0;
  uint32_t ignoreRockerUntil = 0;
  uint32_t repeatAt = 0;
  uint32_t downAt[2] = {0, 0};   // two-button mode: [0] EXIT, [1] MENU
  bool used[2] = {true, true};
  int tapPin = -1;
  uint32_t tapAt = 0;
  auto emit = [](int pin, uint32_t at) {
    g_pressAtMs = at;
    xQueueSend(g_buttonQueue, &pin, 0);
  };

  for (;;) {
    uint32_t now = millis();
    for (int i = 0; i < BUTTON_COUNT; i++) {
      int pin = BUTTON_PINS[i];
      bool raw = (digitalRead(pin) == LOW);
      if (raw != last[i]) {
        last[i] = raw;
        since[i] = now;
      } else if (raw != stable[i] && now - since[i] >= 20) {
        stable[i] = raw;
        if (g_twoButton) {
          if (pin != PIN_EXIT && pin != PIN_MENU) continue;   // the wheel is switched off
          int b = (pin == PIN_EXIT) ? 0 : 1;
          if (raw) {
            downAt[b] = now;
            used[b] = false;
            if (stable[3] && stable[4]) {                     // both together = select
              used[0] = used[1] = true;
              tapPin = -1;
              emit(PIN_CLICK, now);
            }
          } else if (!used[b]) {
            if (tapPin >= 0 && now - tapAt < 380) {            // second tap in time = select
              tapPin = -1;
              emit(PIN_CLICK, now);
            } else {
              tapPin = pin;
              tapAt = now;
            }
          }
          continue;
        }
        if (!raw) continue;
        if (pin == PIN_CLICK) {
          // Pressing the rocker straight down often brushes up or down too.
          heldBackPin = -1;
          ignoreRockerUntil = now + 250;
          g_pressAtMs = now;
          xQueueSend(g_buttonQueue, &pin, 0);
        } else if (pin == PIN_UP || pin == PIN_DOWN) {
          if ((int32_t)(now - ignoreRockerUntil) >= 0) {
            heldBackPin = pin;
            heldBackUntil = now + 70;
          }
        } else {
          g_pressAtMs = now;
          xQueueSend(g_buttonQueue, &pin, 0);
        }
      }
    }
    if (g_twoButton) {
      heldBackPin = -1;
      for (int b = 0; b < 2; b++) {                           // a hold = the button's own job
        if (stable[3 + b] && !used[b] && now - downAt[b] > 550) {
          used[b] = true;
          tapPin = -1;
          emit(b == 0 ? PIN_EXIT : PIN_MENU, now);
        }
      }
      if (tapPin >= 0 && now - tapAt >= 380) {                // a lone tap = up or down
        emit(tapPin == PIN_EXIT ? PIN_DOWN : PIN_UP, now);
        tapPin = -1;
      }
      g_inputBusy = stable[3] || stable[4] || last[3] || last[4] || tapPin >= 0;
      vTaskDelay(pdMS_TO_TICKS(g_inputBusy ? 5 : 20));
      continue;
    }
    // Holding the wheel up or down keeps moving through a list, one row per redraw.
    for (int i = 0; i < 2; i++) {
      if (stable[i] && g_repeatOk && !g_rendering && now - since[i] > 550 &&
          (int32_t)(now - repeatAt) >= 0 && uxQueueMessagesWaiting(g_buttonQueue) == 0) {
        int pin = BUTTON_PINS[i];
        repeatAt = now + 180;
        g_pressAtMs = now;
        xQueueSend(g_buttonQueue, &pin, 0);
      }
    }
    if (heldBackPin >= 0 && (int32_t)(now - heldBackUntil) >= 0) {
      g_pressAtMs = now - 70;
      xQueueSend(g_buttonQueue, &heldBackPin, 0);
      heldBackPin = -1;
    }
    bool busy = heldBackPin >= 0;
    for (int i = 0; i < BUTTON_COUNT; i++) busy = busy || stable[i] || last[i];
    g_inputBusy = busy;
    vTaskDelay(pdMS_TO_TICKS(busy ? 5 : 20));
  }
}

static void forgetPhone() {
  NimBLEDevice::deleteAllBonds();
  delay(200);
  ESP.restart();
}

// Expedition mode: the radio goes quiet and the pager sleeps after a minute,
// waking only for a button. Turning it off starts advertising again.
static void setExpedition(bool on) {
  if (on == g_expedition) return;
  g_expedition = on;
  g_prefs.putBool("exped", on);
  NimBLEServer* server = NimBLEDevice::getServer();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (on) {
    if (server) {
      server->advertiseOnDisconnect(false);
      if (g_connected) server->disconnect(g_connHandle);
    }
    if (adv) adv->stop();
  } else {
    if (server) server->advertiseOnDisconnect(true);
    g_fastAdvUntilMs = millis() + 30000;
  }
  g_lastActivityMs = millis();
  g_forceFull = true;
  if (g_screen == SCREEN_HOME) markDirty(0);
}

static void onSettingsSelect() {
  switch (g_settingsSel) {
    case 0:
      g_smallText = !g_smallText;
      g_prefs.putBool("small", g_smallText);
      g_listTop = 0;
      g_detailTop = 0;
      break;
    case 1:
      g_flip = !g_flip;
      g_prefs.putBool("flip", g_flip);
      Epd::setFlip(g_flip);
      g_forceFull = true;
      break;
    case 2:
      g_fastRefresh = !g_fastRefresh;
      g_prefs.putBool("fast", g_fastRefresh);
      g_forceFull = true;
      break;
    case 3:
      g_mirror = !g_mirror;
      g_prefs.putBool("mirror", g_mirror);
      Epd::setMirror(g_mirror);
      g_forceFull = true;
      break;
    case 4:
      g_sleepChoice = (g_sleepChoice + 1) % SLEEP_CHOICES;
      g_prefs.putUChar("sleepSel", g_sleepChoice);
      break;
    case 5:
      g_checkChoice = (g_checkChoice + 1) % CHECK_CHOICES;
      g_prefs.putUChar("checkSel", g_checkChoice);
      break;
    case 6:
      g_sleepFace = g_sleepFace ? 0 : 1;
      g_prefs.putUChar("sleepFace", g_sleepFace);
      break;
    case 7:
      g_face = (g_face + 1) % FACE_COUNT;
      g_prefs.putUChar("homeFace", g_face);
      break;
    case 8:
      g_popupChoice = (g_popupChoice + 1) % POPUP_CHOICES;
      g_prefs.putUChar("popupSel", g_popupChoice);
      break;
    case 9:
      g_twoButton = !g_twoButton;
      g_prefs.putBool("twoButton", g_twoButton);
      break;
    case 10:
      setReadTurn(g_readTurn + 1);
      break;
    case 11:
      setExpedition(!g_expedition);
      break;
    case 12:
      g_screen = SCREEN_STATUS;
      break;
    case 13:
      g_notes.clear();
      g_sel = 0;
      g_listTop = 0;
      g_screen = SCREEN_LIST;
      g_forceFull = true;
      break;
    default:
      forgetPhone();
      break;
  }
}

static void saveBookmark() {
  g_markItem = g_readItem;
  g_markPage = g_readPage;
  g_prefs.putInt("bkItem", g_markItem);
  g_prefs.putInt("bkPage", g_markPage);
  g_turnsSinceSave = 0;
}

// Opens an entry in the reader and points the browser at it, so EXIT lands
// on the right list.
static void openReader(int item, int page) {
  if (item < 0 || item >= Library::itemCount()) return;
  g_readItem = item;
  g_readPage = page;
  resetImageView();
  for (int b = 0; b < Library::bookCount(); b++) {
    int firstGroup = Library::bookFirstGroup(b);
    for (int g = firstGroup; g < firstGroup + Library::bookGroupCount(b); g++) {
      int firstItem = Library::groupFirstItem(g);
      if (item >= firstItem && item < firstItem + Library::groupItemCount(g)) {
        g_libBook = b;
        g_libGroup = g;
        g_libLevel = 2;
        g_libSel[0] = b + 1;  // row 0 will be "Continue" once a bookmark exists
        g_libSel[1] = g - firstGroup;
        g_libSel[2] = item - firstItem;
      }
    }
  }
  g_screen = SCREEN_READER;
}

static void turnPage(int direction) {
  int firstItem = Library::groupFirstItem(g_libGroup);
  int lastItem = firstItem + Library::groupItemCount(g_libGroup) - 1;
  if (direction > 0) {
    if (g_readPage + 1 < Library::itemPageCount(g_readItem)) {
      g_readPage++;
    } else if (g_readItem < lastItem) {  // carry on into the next entry of this section
      g_readItem++;
      g_readPage = 0;
    } else {
      return;
    }
  } else {
    if (g_readPage > 0) {
      g_readPage--;
    } else if (g_readItem > firstItem) {
      g_readItem--;
      g_readPage = Library::itemPageCount(g_readItem) - 1;
    } else {
      return;
    }
  }
  g_libSel[2] = g_readItem - firstItem;
  resetImageView();
  if (++g_turnsSinceSave >= 5) saveBookmark();
}

static void onLibrarySelect() {
  int sel = g_libSel[g_libLevel];
  if (g_libLevel == 0) {
    if (hasBookmark()) {
      if (sel == 0) {
        openReader(g_markItem, g_markPage);
        return;
      }
      sel--;
    }
    g_libBook = sel;
    g_libLevel = 1;
    g_libSel[1] = g_libTop[1] = 0;
    // A book with a single section and entry opens straight away.
    if (Library::bookGroupCount(g_libBook) == 1 &&
        Library::groupItemCount(Library::bookFirstGroup(g_libBook)) == 1) {
      openReader(Library::groupFirstItem(Library::bookFirstGroup(g_libBook)), 0);
      g_libLevel = 0;
    }
  } else if (g_libLevel == 1) {
    g_libGroup = Library::bookFirstGroup(g_libBook) + sel;
    g_libLevel = 2;
    g_libSel[2] = g_libTop[2] = 0;
  } else {
    openReader(Library::groupFirstItem(g_libGroup) + sel, 0);
  }
}

static void openMainMenu() {
  g_screen = SCREEN_MENU;
  g_menuSel = 0;
}

static void onPress(int pin) {
  if (g_screen == SCREEN_TODAY) {
    if (pin == PIN_MENU) {
      openMainMenu();
      g_menuSel = 1;
    } else {
      g_screen = SCREEN_HOME;
    }
  } else if (g_screen == SCREEN_DECK) {
    int count = (int)g_deckLines.size();
    if (pin == PIN_CLICK) g_deckBack = !g_deckBack;
    if (pin == PIN_DOWN) {
      g_deckAt = (g_deckAt + 1) % count;
      g_deckBack = false;
    }
    if (pin == PIN_UP) {
      g_deckAt = (g_deckAt + count - 1) % count;
      g_deckBack = false;
    }
    if (pin == PIN_EXIT) g_screen = SCREEN_FILES;
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
  } else if (g_screen == SCREEN_MENU) {
    if (pin == PIN_UP && g_menuSel > 0) g_menuSel--;
    if (pin == PIN_DOWN && g_menuSel < MAIN_MENU_COUNT - 1) g_menuSel++;
    if (pin == PIN_EXIT || pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_CLICK) {
      if (g_menuSel == 0) g_screen = SCREEN_LIST;
      if (g_menuSel == 1) g_screen = SCREEN_TODAY;
      if (g_menuSel == 2) {
        g_screen = SCREEN_TODO;
        g_backTo = SCREEN_MENU;
      }
      if (g_menuSel == 3 || g_menuSel == 4) {
        g_group = g_menuSel - 3;
        g_screen = SCREEN_GROUP;
      }
      if (g_menuSel == 5) g_screen = SCREEN_TOOLS;
      if (g_menuSel == 6) {
        g_screen = SCREEN_HOME;
        enterSleep(true);  // does not return
      }
      if (g_menuSel == 7) {
        g_screen = SCREEN_SETTINGS;
        g_settingsSel = 0;
      }
    }
  } else if (g_screen == SCREEN_GROUP) {
    int& sel = g_groupSel[g_group];
    if (pin == PIN_UP && sel > 0) sel--;
    if (pin == PIN_DOWN && sel < GROUP_COUNTS[g_group] - 1) sel++;
    if (pin == PIN_MENU) g_screen = SCREEN_HOME;
    if (pin == PIN_EXIT) {
      openMainMenu();
      g_menuSel = 3 + g_group;
    }
    if (pin == PIN_CLICK) {
      g_backTo = SCREEN_GROUP;
      if (g_group == 0) {
        if (sel == 0) g_screen = SCREEN_LIBRARY;
        if (sel == 1) {
          refreshFileList();
          g_screen = SCREEN_FILES;
        }
        if (sel == 2) {
          findKnots();
          g_screen = SCREEN_KNOTS;
        }
      } else {
        static const Screen TARGETS[4] = {SCREEN_MEDIA, SCREEN_FIND, SCREEN_REMOTE, SCREEN_CARD};
        g_screen = TARGETS[sel];
        if (g_screen == SCREEN_FIND) g_findSent = false;
      }
    }
  } else if (g_screen == SCREEN_LIBRARY) {
    int count = libraryCount();
    int& sel = g_libSel[g_libLevel];
    if (pin == PIN_UP && sel > 0) sel--;
    if (pin == PIN_DOWN && sel < count - 1) sel++;
    if (pin == PIN_CLICK && count > 0) onLibrarySelect();
    if (pin == PIN_MENU) openMainMenu();
    if (pin == PIN_EXIT) {
      if (g_libLevel > 0) {
        g_libLevel--;
      } else {
        g_group = 0;
        g_screen = SCREEN_GROUP;
      }
    }
  } else if (g_screen == SCREEN_READER) {
    int image = readerImage();
    Library::Bitmap fitInfo, zoomInfo;
    bool haveFit = image >= 0 && Library::imageBitmap(image, false, fitInfo);
    bool canZoom = image >= 0 && Library::imageBitmap(image, true, zoomInfo);
    if (image >= 0 && g_imageZoom) {
      // Zoomed in: the rocker scrolls, MENU moves sideways, press or EXIT zooms out.
      if (pin == PIN_DOWN) g_imageY += IMAGE_SCROLL_STEP;
      if (pin == PIN_UP) g_imageY -= IMAGE_SCROLL_STEP;
      if (pin == PIN_MENU) g_imageXStep = (g_imageXStep + 1) % 3;
      if (pin == PIN_CLICK || pin == PIN_EXIT) resetImageView();
    } else {
      // A drawing taller than the screen scrolls before the page turns.
      bool moreBelow = haveFit && fitInfo.height - g_imageY > IMAGE_VIEW_H;
      if (pin == PIN_DOWN) {
        if (moreBelow) g_imageY += IMAGE_SCROLL_STEP;
        else turnPage(+1);
      }
      if (pin == PIN_UP) {
        if (haveFit && g_imageY > 0) g_imageY -= IMAGE_SCROLL_STEP;
        else turnPage(-1);
      }
      if (pin == PIN_CLICK && canZoom) {
        g_imageZoom = true;
        g_imageY = 0;
        g_imageXStep = 0;
      } else if (pin == PIN_CLICK || pin == PIN_EXIT || pin == PIN_MENU) {
        saveBookmark();
        g_screen = (pin == PIN_MENU) ? SCREEN_HOME : SCREEN_LIBRARY;
      }
    }
  } else if (g_screen == SCREEN_HOME && g_face == 7 && !g_expedition && pin != PIN_MENU && pin != PIN_EXIT &&
             todoPress(pin)) {
    // the to-do face keeps the wheel for its own list
  } else if (g_screen == SCREEN_HOME) {
    if (pin == PIN_CLICK || pin == PIN_UP) g_screen = SCREEN_LIST;
    if (pin == PIN_DOWN) g_screen = SCREEN_MEDIA;
    if (pin == PIN_MENU) openMainMenu();
    if (pin == PIN_EXIT) g_forceFull = true;  // clean up any ghosting
  } else if (g_screen == SCREEN_MEDIA) {
    if (pin == PIN_CLICK) sendMediaCommand(2);
    if (pin == PIN_UP) sendMediaCommand(g_mediaSkipMode ? 3 : 5);
    if (pin == PIN_DOWN) sendMediaCommand(g_mediaSkipMode ? 4 : 6);
    if (pin == PIN_MENU) g_mediaSkipMode = !g_mediaSkipMode;
    if (pin == PIN_EXIT) g_screen = SCREEN_HOME;
    if (pin != PIN_MENU && pin != PIN_EXIT) return;  // the phone's reply redraws the screen
  } else if (g_screen == SCREEN_LIST) {
    if (pin == PIN_UP && g_sel > 0) g_sel--;
    if (pin == PIN_DOWN && g_sel < (int)g_notes.size() - 1) g_sel++;
    if (pin == PIN_CLICK) openDetail(g_sel);
    if (pin == PIN_MENU) openMainMenu();
    if (pin == PIN_EXIT) g_screen = SCREEN_HOME;
    clampSelection();
  } else if (g_screen == SCREEN_DETAIL) {
    if (pin == PIN_UP) g_detailTop -= bodyLinesVisible() - 1;
    if (pin == PIN_DOWN) g_detailTop += bodyLinesVisible() - 1;
    if (pin == PIN_CLICK || pin == PIN_EXIT) g_screen = g_detailReturn;
    if (pin == PIN_MENU) {
      // Dismiss on the phone as well; the phone then tells us it is gone.
      uint32_t uid = g_detailUid;
      performAction(uid, 0x01);
      removeNote(uid);
      g_screen = g_detailReturn;
    }
  } else if (isToolScreen()) {
    if (!onToolPress(pin)) return;
  } else if (g_screen == SCREEN_UPDATE) {
    return;  // buttons do nothing while firmware installs
  } else if (g_screen == SCREEN_FILES) {
    int count = (int)g_fileNames.size();
    if (pin == PIN_UP && g_fileSel > 0) g_fileSel--;
    if (pin == PIN_DOWN && g_fileSel < count - 1) g_fileSel++;
    if (pin == PIN_CLICK && count > 0) {
      if (g_fileNames[g_fileSel].endsWith(".cards")) {
        if (openDeck(g_fileNames[g_fileSel])) g_screen = SCREEN_DECK;
      } else if (openNote(g_fileNames[g_fileSel])) {
        g_screen = SCREEN_NOTE;
      }
    }
    if (pin == PIN_MENU) openMainMenu();
    if (pin == PIN_EXIT) {
      g_group = 0;
      g_screen = SCREEN_GROUP;
    }
  } else if (g_screen == SCREEN_NOTE) {
    int pages = (int)g_notePages.size();
    if (pin == PIN_DOWN && g_notePage < pages - 1) g_notePage++;
    if (pin == PIN_UP && g_notePage > 0) g_notePage--;
    if (pin == PIN_CLICK || pin == PIN_EXIT || pin == PIN_MENU) {
      saveNoteSpot();
      if (g_noteFile) g_noteFile.close();
      g_screen = (pin == PIN_MENU) ? SCREEN_HOME : SCREEN_FILES;
    }
  } else if (g_screen == SCREEN_STATUS) {
    if (pin == PIN_UP || pin == PIN_DOWN || pin == PIN_CLICK) {
      g_statusPage = g_statusPage ? 0 : 1;  // Status <-> Link log
    } else {
      g_screen = (pin == PIN_EXIT) ? SCREEN_HOME : SCREEN_SETTINGS;
      g_statusPage = 0;
    }
  } else {
    if (pin == PIN_UP && g_settingsSel > 0) g_settingsSel--;
    if (pin == PIN_DOWN && g_settingsSel < SETTINGS_COUNT - 1) g_settingsSel++;
    if (pin == PIN_CLICK) onSettingsSelect();
    if (pin == PIN_EXIT || pin == PIN_MENU) g_screen = SCREEN_HOME;
  }
  markDirty(0);
}

static void serviceButtons() {
  int pin;
  while (xQueueReceive(g_buttonQueue, &pin, 0) == pdTRUE) {
    g_lastActivityMs = millis();
    g_popupActive = false;  // touched: the notification stays until closed by hand
    if (g_quiet) {
      wakeScreen();  // the press only wakes the screen
      continue;
    }
    onPress(pin);
  }
}

// ------------------------------------------------------------------ sleep

static String clockText() {
  ClockNow now;
  if (!clockNow(now)) return "";
  int hour12 = now.hour % 12;
  if (hour12 == 0) hour12 = 12;
  return String(hour12) + ":" + (now.minute < 10 ? "0" : "") + String(now.minute) +
         (now.hour < 12 ? " AM" : " PM");
}

static void startDisplay(bool switchPanel) {
  PanelType forced = (PanelType)g_prefs.getUChar("panel", PANEL_AUTO);
  PanelType active = Epd::begin(forced);
  if (switchPanel) {
    // The screen stayed blank, so try the other panel type from now on.
    forced = (active == PANEL_V12) ? PANEL_SSD1680 : PANEL_V12;
    g_prefs.putUChar("panel", forced);
    Epd::begin(forced);
    Serial.printf("[pager] panel type switched to %d\n", forced);
  }
  Epd::setFlip(g_flip);
  Epd::setMirror(g_mirror);
  g_displayStarted = true;
}

// Ends a quiet phone check and brings the screen back to life.
static void wakeScreen() {
  if (!g_quiet) return;
  g_quiet = false;
  if (!g_displayStarted) startDisplay(false);
  if (g_rtc.resumeReader && hasBookmark()) openReader(g_markItem, g_markPage);
  g_forceFull = true;
  g_lastActivityMs = millis();
  markDirty(0);
}

// What stays on the screen while the pager is asleep.
static void drawSleepFace() {
  g_canvas.fillScreen(0);
  String when = clockText();
  drawHeader("PAGER - ASLEEP", when.length() ? "since " + when : String(""));
  g_canvas.setFont(nullptr);
  g_canvas.setTextSize(1);
  g_canvas.setTextColor(1);

  int unread = unreadCount();
  int show = -1;
  if (g_screen == SCREEN_DETAIL) show = findNote(g_detailUid);
  for (size_t i = 0; show < 0 && i < g_notes.size(); i++) {
    if (g_notes[i].unread) show = (int)i;
  }

  if (show >= 0) {
    const Note& note = g_notes[show];
    String top = unread > 0 ? String(unread) + " new" : String("Latest");
    top += "  -  " + note.app;
    g_canvas.setCursor(5, HEADER_H + 4);
    g_canvas.print(top.substring(0, 40));
    g_canvas.setCursor(5, HEADER_H + 16);
    g_canvas.print(note.title.substring(0, 40));
    g_canvas.setCursor(6, HEADER_H + 16);  // drawn twice, one pixel apart = bold
    g_canvas.print(note.title.substring(0, 40));
    std::vector<String> lines;
    String body = note.body;
    body.replace('\n', ' ');
    wrapText(body, EPD_WIDTH - 10, lines);
    for (size_t i = 0; i < lines.size() && i < 5; i++) {
      g_canvas.setCursor(5, HEADER_H + 29 + (int)i * 10);
      g_canvas.print(lines[i]);
    }
  } else {
    g_canvas.setFont(&FreeSansBold24pt7b);
    drawCentered("Asleep", 62);
    drawSmallCentered(g_notes.empty() ? "No notifications" : "No new notifications", 76);
    int minutes = CHECK_MINUTES[g_checkChoice];
    drawSmallCentered(minutes ? "Checks the phone every " + minutesLabel(minutes)
                              : String("Not checking the phone while asleep"), 90);
  }
  g_canvas.drawFastHLine(0, EPD_HEIGHT - 13, EPD_WIDTH, 1);
  drawSmallCentered("Press any button to wake", EPD_HEIGHT - 10);
}

// Powers everything down. The chip restarts from setup() when it wakes.
// drawFace = false leaves the screen exactly as it is.
static void enterSleep(bool drawFace) {
  Serial.println("[sleep] going to sleep");
  if (drawFace) {
    if (!g_displayStarted) startDisplay(false);
    bool reading = (g_screen == SCREEN_READER);
    g_rtc.resumeReader = reading;
    if (reading) {
      // Leave the page up so reading can carry on; just label it.
      saveBookmark();
      g_canvas.fillScreen(0);
      drawReader();
      drawHeader("ASLEEP - press any button to wake", "");
      Epd::show(g_canvas.getBuffer(), g_forceFull || !g_fastRefresh);
    } else {
      if (g_sleepFace == 1) drawCard(true);
      else drawSleepFace();
      Epd::show(g_canvas.getBuffer(), true);
    }
  }

  // Keep the clock running through the sleep.
  g_rtc.timeValid = g_timeKnown;
  if (g_timeKnown) {
    struct timeval tv = {};
    tv.tv_sec = (time_t)(g_timeBase + (uint32_t)(millis() - g_timeBaseMs) / 1000);
    settimeofday(&tv, nullptr);
  }

  // Say goodbye to the phone properly so it reconnects quickly later.
  g_sleeping = true;
  bool paired = NimBLEDevice::getNumBonds() > 0;
  NimBLEServer* server = NimBLEDevice::getServer();
  if (server) {
    server->advertiseOnDisconnect(false);
    if (g_connected) server->disconnect(g_connHandle);
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv) adv->stop();
  delay(300);

  // A button still held down would wake it again straight away.
  uint32_t start = millis();
  while (millis() - start < 3000) {
    bool held = false;
    for (int pin : BUTTON_PINS) held = held || (digitalRead(pin) == LOW);
    if (!held) break;
    delay(10);
  }
  delay(60);

  digitalWrite(PIN_LED, LOW);
  gpio_hold_en((gpio_num_t)PIN_LED);
  Epd::powerOff();
  gpio_deep_sleep_hold_en();

  uint64_t mask = 0;
  for (int pin : BUTTON_PINS) {
    mask |= 1ULL << pin;
    rtc_gpio_init((gpio_num_t)pin);
    rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
    rtc_gpio_pullup_en((gpio_num_t)pin);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // keeps the pull-ups working
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  int minutes = CHECK_MINUTES[g_checkChoice];
  if (minutes > 0 && paired && !g_expedition) esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// Sleeps once nothing has happened for the chosen time.
// Puts the screen back where it was when a pop-up notification has been left alone.
static void servicePopup() {
  if (!g_popupActive) return;
  if (g_screen != SCREEN_DETAIL || g_detailUid != g_popupUid) {
    g_popupActive = false;
    return;
  }
  int seconds = POPUP_SECONDS[g_popupChoice];
  if (!seconds || millis() - g_popupAtMs < (uint32_t)seconds * 1000UL) return;
  g_popupActive = false;
  int idx = findNote(g_popupUid);
  if (idx >= 0) g_notes[idx].unread = true;   // nobody pressed anything, so it still counts as new
  g_screen = g_detailReturn;
  g_forceFull = true;
  markDirty(0);
}

static void serviceIdle() {
  int minutes = g_expedition ? 1 : SLEEP_MINUTES[g_sleepChoice];
  if (minutes == 0) return;
  if (millis() - g_lastActivityMs < (uint32_t)minutes * 60000UL) return;
  if (g_dirty || g_fetching || !g_pending.empty()) return;
  if (g_timerRunning || g_timerBlinking) return;  // a sleep would lose the countdown
  if (g_xferKind) return;                          // a file or firmware is arriving
  if (g_swRunning && stopwatchMs() < 3UL * 3600UL * 1000UL) return;
  if (!g_expedition && NimBLEDevice::getNumBonds() == 0) return;  // still being paired
  enterSleep(true);
}

// Runs after the pager wakes by itself: connect, see whether anything new
// arrived, then either wake the screen or go straight back to sleep.
static void serviceQuietCheck() {
  uint32_t up = millis();
  bool settled = g_ancsReady && !g_fetching && g_pending.empty() &&
                 up - g_readyAtMs > 3000 && up - g_lastNsMs > 2500;
  bool gaveUp = (!g_connected && up > 20000) || (!g_ancsReady && up > 40000) || up > 75000;
  if (!settled && !gaveUp) return;

  if (g_newSinceSleep > 0) {
    Serial.printf("[sleep] %d new while asleep, waking\n", g_newSinceSleep);
    g_rtc.resumeReader = 0;
    wakeScreen();
    g_screen = SCREEN_LIST;
    g_sel = 0;
    for (size_t i = 0; i < g_notes.size(); i++) {
      if (g_notes[i].unread) {
        g_sel = (int)i;
        break;
      }
    }
    clampSelection();
    blinkLed(3);
    return;
  }
  Serial.println(settled ? "[sleep] nothing new" : "[sleep] phone not reachable");
  enterSleep(false);
}

// ------------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[pager] starting");

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  g_fromSleep = (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_TIMER);
  g_quiet = (cause == ESP_SLEEP_WAKEUP_TIMER);
  if (g_fromSleep) Serial.println(g_quiet ? "[sleep] woke to check the phone" : "[sleep] woken by a button");

  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_LED);
  for (int pin : BUTTON_PINS) rtc_gpio_deinit((gpio_num_t)pin);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  for (int pin : BUTTON_PINS) pinMode(pin, INPUT_PULLUP);
  delay(20);

  g_prefs.begin("pager", false);
  g_flip = g_prefs.getBool("flip", false);
  g_readTurn = g_prefs.getUChar("readTurn", 0) % 3;
  g_expedition = g_prefs.getBool("exped", false);
  loadWaypoints();
  g_fastRefresh = g_prefs.getBool("fast", true);
  g_mirror = g_prefs.getBool("mirror", true);
  g_smallText = g_prefs.getBool("small", true);
  g_markItem = g_prefs.getInt("bkItem", -1);
  g_markPage = g_prefs.getInt("bkPage", 0);
  g_sleepChoice = g_prefs.getUChar("sleepSel", 0);
  g_checkChoice = g_prefs.getUChar("checkSel", 2);
  if (g_sleepChoice >= SLEEP_CHOICES) g_sleepChoice = 0;
  if (g_checkChoice >= CHECK_CHOICES) g_checkChoice = 2;
  g_sleepFace = g_prefs.getUChar("sleepFace", 0) ? 1 : 0;
  g_twoButton = g_prefs.getBool("twoButton", false);
  g_popupChoice = g_prefs.getUChar("popupSel", 2);
  if (g_popupChoice >= POPUP_CHOICES) g_popupChoice = 2;
  g_tally = g_prefs.getInt("tally", 0);
  {
    String corners = g_prefs.getString("corners", "1236");
    for (int i = 0; i < 4 && i < (int)corners.length(); i++) {
      g_corners[i] = (uint8_t)constrain(corners[i] - '0', 0, 9);
    }
  }
  g_snBest = g_prefs.getInt("snBest", 0);
  g_tBest = g_prefs.getUInt("t48Best", 0);
  g_face = g_prefs.getUChar("homeFace", 0);
  if (g_face >= FACE_COUNT) g_face = 0;
  g_cardName = g_prefs.getString("cName", CARD_NAME);
  g_cardLine1 = g_prefs.getString("cLine1", CARD_LINE1);
  g_cardLine2 = g_prefs.getString("cLine2", CARD_LINE2);
  g_cardQr = g_prefs.getString("cQr", CARD_QR);
  g_layoutChanged = g_prefs.getUChar("gattv", 1) != GATT_LAYOUT;
  g_fsReady = LittleFS.begin(true, "/files", 8, "spiffs");  // formats the storage on first use
  Serial.printf("[app] file storage %s\n", g_fsReady ? "ready" : "NOT available");
  loadTodo();
  g_timerSetSec = g_prefs.getInt("timerSec", 300);
  if (g_timerSetSec < 60 || g_timerSetSec > 180 * 60) g_timerSetSec = 300;
  g_timerLeftSec = g_timerSetSec;
  bool libraryOk = Library::begin(pager_library_start, pager_library_end - pager_library_start);
  Serial.printf("[library] %s: %d books, %d entries\n", libraryOk ? "ok" : "NOT VALID",
                Library::bookCount(), Library::itemCount());

  // The power-on shortcuts must not fire when a button is what woke it up.
  bool menuHeld = !g_fromSleep && (digitalRead(PIN_MENU) == LOW);
  bool exitHeld = !g_fromSleep && (digitalRead(PIN_EXIT) == LOW);
  bool menuHeldAtBoot = !g_fromSleep && (digitalRead(PIN_MENU) == LOW);

  if (g_fromSleep && g_rtc.timeValid) {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    g_timeBase = tv.tv_sec;
    g_timeBaseMs = millis();
    g_timeKnown = true;
  }

  // A quiet phone check leaves the screen alone unless something turns up.
  if (!g_quiet) {
    startDisplay(menuHeld);
    if (g_fromSleep && g_rtc.resumeReader && hasBookmark()) openReader(g_markItem, g_markPage);
  }

  // Lines are wrapped by wrapText(); the library's own wrapping would both
  // mis-measure long lines and spill them onto the next row.
  g_canvas.setTextWrap(false);

  g_nsQueue = xQueueCreate(48, sizeof(NsEvent));
  g_requestQueue = xQueueCreate(8, sizeof(BleRequest));
  g_buttonQueue = xQueueCreate(8, sizeof(int));
  g_infoQueue = xQueueCreate(12, sizeof(InfoEvent));
  g_dsStream = xStreamBufferCreate(2048, 1);

  startBluetooth();
  if (exitHeld) {
    Serial.println("[pager] forgetting paired phone");
    NimBLEDevice::deleteAllBonds();
  }

  setCpuFrequencyMhz(80);
  startPowerSaving(menuHeldAtBoot);
  xTaskCreatePinnedToCore(inputTask, "input", 3072, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(bluetoothTask, "pager-ble", 8192, nullptr, 2, nullptr, 1);
  g_lastActivityMs = millis();
  markDirty(0);
}

// Switch napping on, unless the last few starts crashed or MENU is held at
// power-on (the escape hatch if power saving ever misbehaves).
static void startPowerSaving(bool skip) {
  esp_reset_reason_t why = esp_reset_reason();
  bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                 why == ESP_RST_WDT || why == ESP_RST_BROWNOUT;
  uint8_t bad = g_prefs.getUChar("badBoots", 0);
  if (crashed) {
    if (bad < 250) bad++;
    g_prefs.putUChar("badBoots", bad);
  }
  g_bootCounted = bad > 0;
  if (skip || bad >= 3) {
    Serial.printf("[power] saver off this boot (skip %d, bad boots %u)\n", (int)skip, (unsigned)bad);
    return;
  }
  esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "inkfob", &g_awakeLock);
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = 80;
  cfg.min_freq_mhz = 80;
  cfg.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&cfg);
  g_saverOn = (err == ESP_OK);
  Serial.printf("[power] saver %s (%d)\n", g_saverOn ? "on" : "failed", (int)err);
}

// Keeps the chip awake only while it has to be, and asks the phone for a
// relaxed link when idle and a quick one while a file or update is arriving.
static void servicePower() {
  uint32_t now = millis();
  if (g_bootCounted && now > 90000) {          // ran fine for a while: clear the crash count
    g_bootCounted = false;
    g_prefs.putUChar("badBoots", 0);
  }
  bool busy = g_xferKind != 0;
  stayAwake(busy);
  if (!g_connected) return;
  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server) return;
  if (g_linkAskedGen != g_connGeneration) {    // a fresh connection starts quick
    g_linkAskedGen = g_connGeneration;
    g_linkSlow = false;
    g_linkAskAtMs = now + 25000;
  }
  if (busy) {
    if (g_linkSlow) {
      server->updateConnParams(g_connHandle, 12, 24, 0, 400);      // 15-30 ms
      g_linkSlow = false;
    }
    g_linkAskAtMs = now + 8000;
  } else if (g_saverOn && !g_linkSlow && !g_fetching && (int32_t)(now - g_linkAskAtMs) >= 0) {
    server->updateConnParams(g_connHandle, 144, 168, 2, 600);      // 180-210 ms, may skip 2
    g_linkSlow = true;
  }
}

// Rest until the next button press or for a short while, whichever is first.
static void idleWait(uint32_t ms) {
  int pin;
  xQueuePeek(g_buttonQueue, &pin, pdMS_TO_TICKS(ms));
}

void loop() {
  serviceButtons();
  serviceBluetooth();
  serviceInfo();
  serviceAppLink();
  if (g_quiet) {
    serviceQuietCheck();
    servicePower();
    idleWait(40);
    return;
  }
  g_repeatOk = !g_dirty && (g_screen == SCREEN_MENU || g_screen == SCREEN_LIST || g_screen == SCREEN_GROUP ||
                            g_screen == SCREEN_LIBRARY || g_screen == SCREEN_SETTINGS ||
                            g_screen == SCREEN_TOOLS || g_screen == SCREEN_FILES ||
                            g_screen == SCREEN_GAMES || g_screen == SCREEN_TIP ||
                            g_screen == SCREEN_CONVERT || g_screen == SCREEN_HANGMAN ||
                            g_screen == SCREEN_MINES || g_screen == SCREEN_MORSE);
  serviceTools();
  servicePopup();
  serviceIdle();
  if (g_dirty && (int32_t)(millis() - g_dirtyAtMs) >= 0) {
    // While a batch of notifications is still loading, wait so the screen is
    // drawn once at the end (but never keep it stale for more than 3 seconds).
    bool loading = g_fetching || !g_pending.empty();
    if (!loading || g_dirtyUrgent || millis() - g_dirtySinceMs > 3000) {
      g_dirty = false;
      g_dirtyUrgent = false;
      g_rendering = true;
      stayAwake(true);
      render();
      g_rendering = false;
    }
  }
  servicePower();
  idleWait(g_screen == SCREEN_SNAKE || g_dirty ? 10 : 40);
}
