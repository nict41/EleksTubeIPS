#ifndef TFTS_H
#define TFTS_H

// Call up the LittleFS FLASH filing system this is part of the ESP Core
#define FS_NO_GLOBALS
#include <FS.h>
#include <TFT_eSPI.h>
#include "ChipSelect.h"
#include "DigitalRainAnimation.h"

#define TFT_PWM_CHANNEL 0
#define TFT_PWM_FREQ 20000   // PWM frequency for TFT dimming (Hz)
#define TFT_PWM_RESOLUTION 8 // PWM resolution for TFT dimming (bits)

class StaticSprite : public TFT_eSprite {
public:
  StaticSprite(TFT_eSPI *tft) : TFT_eSprite(tft) {}
  void pushImageWithTransparency(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data, uint16_t transparentColor = 0);
  void pushImageWithAlpha(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data, uint8_t *alpha, uint8_t opaque);
  void init();

  static uint8_t output_buffer[(TFT_HEIGHT * TFT_WIDTH + 1) * sizeof(uint16_t)];
};

struct MaskData {
  uint16_t rMask = 0xf800;
  uint16_t gMask = 0x07e0;
  uint16_t bMask = 0x001f;
  uint16_t aMask = 0;
  uint8_t rShift = 11;
  uint8_t gShift = 5;
  uint8_t bShift = 0;
  uint8_t aShift = 0;
};

class TFTs : public TFT_eSPI {
public:
  TFTs() : TFT_eSPI(), chip_select(), enabled(false)
    {}

  // no == Do not send to TFT. yes == Send to TFT if changed. force == Send to TFT.
  enum show_t { no, yes, force };
  enum image_justification_t { TOP_LEFT, TOP_CENTER, TOP_RIGHT, MIDDLE_LEFT, MIDDLE_CENTER, MIDDLE_RIGHT, BOTTOM_LEFT, BOTTOM_CENTER, BOTTOM_RIGHT };
#ifdef TFTS_FX
  enum GRAPHICS_FX {
    NONE = 0,
    GREYSCALE,
    TINT
  };

  static ByteConfigItem& getFx() { static ByteConfigItem fx("fx", TINT); return fx; }
#endif
  static const char* INVALID_DIGIT;
  // The "digit" a panel tube shows: not a file, but a picture streamed over HTTP, see /panel
  static const char* PANEL_DIGIT;

  void claim();
  void release();

  void begin(fs::FS& fs);
  void clear();
  void setShowDigits(byte);

  void setDigit(uint8_t digit, const char* name, show_t show=yes);
  const char* getDigitName(uint8_t index) { return icons[index]; }

  void invalidateAllDigits();

  void showAllDigits() { for (uint8_t digit=0; digit < NUM_DIGITS; digit++) showDigit(digit); }
  void showDigit(uint8_t digit);
  TFT_eSprite& drawImage(uint8_t digit);
  StaticSprite& getSprite();

  // Panel tubes, whose pictures come over HTTP rather than from files. Panel 0..5 is the tube
  // left to right, i.e. HOURS_TENS..SECONDS_ONES, the same order as the DIGITS array in IPSClock.cpp.
  static uint8_t panelToDigit(uint8_t panel);   // The digit constant of a panel index
  bool isPanelTube(uint8_t panel);              // True when that tube is showing a panel picture
  uint8_t getPanelTubes();                      // Bit i set when panel i is a panel tube
  uint8_t getPanelsNeeded();                    // Bit i set when panel i needs a fresh picture
  void clearPanelNeeded(uint8_t panel);
  void setPanelNeeded(uint8_t panel);           // An unfinished picture: the tube needs a fresh one
  // Draws h full rows of w pixels at (x, y) on a panel tube. rgb565be holds w*h pixels in RGB565,
  // big-endian (high byte first), row by row.
  void drawPanelRows(uint8_t panel, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *rgb565be);
  void drawStatus();                            // Draws the status message on the selected tube, if any

  void animateRain();

  void setImageJustification(image_justification_t value) { imageJustification = value; }
  void setBox(uint16_t w, uint16_t h) { boxWidth = w; boxHeight = h; }
  // Controls the power to all displays
  void enableAllDisplays();
  void disableAllDisplays();
  void toggleAllDisplays()           { if (enabled) disableAllDisplays(); else enableAllDisplays(); }
  bool isEnabled()                   { return enabled; }
  void setDimming(uint8_t dimming);

  // Making chip_select public so we don't have to proxy all methods, and the caller can just use it directly.
  ChipSelect chip_select;

  void setStatus(const char*);
  void setStatus(const String& s) { setStatus(s.c_str()); }
  // A small label above a value in bigger text. The value can have line breaks, or if split, a value
  // too long for one line can go on more.
  void setStatus(const char *label, const char *value, bool split = false);
  bool checkStatus();
  void printlnAll(const char* s);
  void printAll(const char* s);
  void drawMeter(int val, bool first, const char *legend);

  uint16_t dimColor(uint16_t pixel);
  void setMonochromeColor(int color);

private:
  static SemaphoreHandle_t tftMutex;

  TFT_eSprite& getStatusSprite(int16_t height);
  void showStatus(unsigned long duration);
#ifdef SMOOTH_FONT
  DigitalRainAnim& getMatrixAnimator();
#else
  DigitalRainAnimation& getMatrixAnimator();
#endif

  byte showDigits = 0;
  image_justification_t imageJustification = MIDDLE_CENTER;
  uint16_t boxWidth = TFT_WIDTH;
  uint16_t boxHeight = TFT_HEIGHT;
  uint8_t dimming = 255; // amount of dimming graphics
  int monochromeColor = -1;

  // Bit i set when panel tube i needs a fresh picture. Touched from the clock task and the web
  // server task, so only changed while holding the mutex, see claim()/release().
  volatile uint8_t panelsNeeded = 0;

  unsigned long statusTime = 0;
  unsigned long statusDuration = 5000;
  bool statusSet = false;
  bool statusShrunk = false;

  uint16_t defaultTextColor = TFT_WHITE;
  uint16_t defaultTextBackground = TFT_BLACK;
  uint8_t current_graphic = 1;
  char icons[NUM_DIGITS][16];

  bool enabled;
  fs::FS* fs;

  bool LoadImageIntoBuffer(const char* filename);
  bool LoadBMPImageIntoBuffer(fs::File &file);
  bool LoadCLKImageIntoBuffer(fs::File &file);
  bool LoadImageBytesIntoSprite(int16_t w, int16_t h, uint8_t bpp, int16_t rowSize, bool reversed, MaskData *pMaskData, uint32_t *palette, fs::File &file);

  uint16_t read16(fs::File &f);
  uint32_t read32(fs::File &f);

  uint8_t FileInBuffer=255; // invalid, always load first image
  uint8_t NextFileRequired = 0; 
};

extern TFTs *tfts;


#endif // TFTS_H
