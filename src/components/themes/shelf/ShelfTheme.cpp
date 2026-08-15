#include "ShelfTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <WiFi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "I18n.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/apps.h"
#include "components/icons/folder.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/book.h"
#include "components/icons/wifi.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
constexpr int kHeroCoverWidth = 240;
constexpr int kHeroPadding = 20;
constexpr int kGridCoverWidth = 90;
constexpr int kGridCoverHeight = 130;
constexpr int kGridRows = 2;
constexpr int kGridCols = 5;
constexpr int kMenuIconSize = 32;

void drawShelfCover(const GfxRenderer& renderer, const RecentBook& book, int x, int y, int width, int height, int thumbHeight, bool selected) {
  bool hasCover = false;
  if (!book.coverBmpPath.empty()) {
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const float scale = std::min({1.0f, static_cast<float>(width) / bitmap.getWidth(), static_cast<float>(height) / bitmap.getHeight()});
        const int drawWidth = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
        const int drawHeight = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
        const int drawX = x + (width - drawWidth) / 2;
        const int drawY = y + (height - drawHeight) / 2;
        renderer.drawBitmap(bitmap, drawX, drawY, drawWidth, drawHeight);
        hasCover = true;
      }
    }
  }

  if (!hasCover) {
    renderer.drawRect(x, y, width, height, true);
    UITheme::drawCenteredWrappedText(renderer, Rect{x + 5, y + 10, width - 10, height - 20}, SMALL_FONT_ID, book.title.c_str(), 5);
  }

  if (selected) {
    renderer.drawRect(x - 2, y - 2, width + 4, height + 4, true);
    renderer.drawRect(x - 3, y - 3, width + 6, height + 6, true);
  }
}
}  // namespace

void ShelfTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                     const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                     bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  if (!coverRendered) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    const int heroHeight = ShelfMetrics::values.homeCoverHeight;

    // 1. Hero Book
    const RecentBook& hero = recentBooks[0];
    const bool heroSelected = selectorIndex == 0;
    int heroX = rect.x + kHeroPadding;
    int heroY = rect.y + 10;
    drawShelfCover(renderer, hero, heroX, heroY, kHeroCoverWidth, heroHeight, heroHeight, heroSelected);

    // Hero Info
    int infoX = heroX + kHeroCoverWidth + kHeroPadding;
    int infoY = heroY + 20;
    int infoWidth = rect.width - infoX - kHeroPadding;

    renderer.drawText(UI_12_FONT_ID, infoX, infoY, renderer.truncatedText(UI_12_FONT_ID, hero.title.c_str(), infoWidth, EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
    infoY += renderer.getLineHeight(UI_12_FONT_ID) + 10;

    renderer.drawText(UI_10_FONT_ID, infoX, infoY, renderer.truncatedText(UI_10_FONT_ID, hero.author.c_str(), infoWidth).c_str());
    infoY += renderer.getLineHeight(UI_10_FONT_ID) + 20;

    // Progress
    const auto* stats = READING_STATS.findBook(hero.path);
    int progress = stats ? stats->lastProgressPercent : 0;

    char progBuf[32];
    snprintf(progBuf, sizeof(progBuf), "%d%%", progress);
    renderer.drawText(SMALL_FONT_ID, infoX, infoY, progBuf);

    int barX = infoX + renderer.getTextWidth(SMALL_FONT_ID, "100%") + 10;
    int barWidth = rect.width - barX - kHeroPadding;
    int barHeight = 6;
    renderer.drawRect(barX, infoY + 6, barWidth, barHeight, true);
    renderer.fillRect(barX, infoY + 6, barWidth * progress / 100, barHeight, true);

    // 2. Grid of Books
    int gridStartY = heroY + heroHeight + 40;
    int cellWidth = rect.width / kGridCols;
    int cellHeight = (rect.height - (gridStartY - rect.y)) / kGridRows;

    for (size_t i = 1; i < recentBooks.size() && i <= 10; ++i) {
      int idx = i - 1;
      int row = idx / kGridCols;
      int col = idx % kGridCols;
      bool selected = (int)i == selectorIndex;

      int x = rect.x + col * cellWidth + (cellWidth - kGridCoverWidth) / 2;
      int y = gridStartY + row * cellHeight + 10;

      // Pass heroHeight for path lookup, but kGridCoverHeight for layout
      drawShelfCover(renderer, recentBooks[i], x, y, kGridCoverWidth, kGridCoverHeight, heroHeight, selected);

      // Author label if unknown
      if (recentBooks[i].author.empty() || recentBooks[i].author == "Unknown") {
          int labelY = y + kGridCoverHeight - 30;
          renderer.fillRect(x, labelY, kGridCoverWidth, 30, false);
          renderer.drawRect(x, labelY, kGridCoverWidth, 30, true);
          renderer.fillRectDither(x, labelY, kGridCoverWidth, 30, Color::LightGray);
          UITheme::drawCenteredText(renderer, Rect{x, labelY, kGridCoverWidth, 30}, SMALL_FONT_ID, labelY + (30 - renderer.getLineHeight(SMALL_FONT_ID))/2, "Unknown Author");
      }
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
}

void ShelfTheme::drawHomeMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width, rect.y, true);

  int itemWidth = rect.width / buttonCount;
  for (int i = 0; i < buttonCount; ++i) {
    int x = rect.x + i * itemWidth;
    bool selected = i == selectedIndex;

    const uint8_t* icon = iconForName(rowIcon(i), kMenuIconSize);
    if (icon) {
      int iconX = x + (itemWidth - kMenuIconSize) / 2;
      int iconY = rect.y + 15;
      if (selected) {
          renderer.fillRoundedRect(iconX - 10, iconY - 5, kMenuIconSize + 20, kMenuIconSize + 40, 8, Color::Black);
          renderer.drawIconInverted(icon, iconX, iconY, kMenuIconSize);
      } else {
          renderer.drawIcon(icon, iconX, iconY, kMenuIconSize);
      }
    }

    std::string label = buttonLabel(i);
    int labelY = rect.y + 15 + kMenuIconSize + 5;
    UITheme::drawCenteredText(renderer, Rect{x, labelY, itemWidth, 20}, SMALL_FONT_ID, labelY, label.c_str(), !selected);
  }
}

void ShelfTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  (void)title;
  (void)subtitle;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  // Time
  char timeBuf[16];
  TimeUtils::formatCurrentTime(timeBuf, sizeof(timeBuf), SETTINGS.clockFormat == 1);
  renderer.drawText(UI_10_FONT_ID, rect.x + 10, rect.y + 10, timeBuf, true, EpdFontFamily::BOLD);

  // Status icons
  int iconX = rect.x + rect.width - 40;
  if (WiFi.status() == WL_CONNECTED) {
      renderer.drawIcon(WifiIcon, iconX - 30, rect.y + 10, 32);
      iconX -= 40;
  }

  drawBatteryRight(renderer, Rect{rect.x + rect.width - 60, rect.y + 10, 40, 20}, true);
}

bool ShelfTheme::homeIndexFromPoint(const GfxRenderer& renderer, int x, int y, int bookCount, int menuCount,
                                    int& index) const {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto& metrics = ShelfMetrics::values;

  // 1. Check Hero Book
  int heroX = metrics.contentSidePadding + kHeroPadding;
  int heroY = metrics.homeTopPadding + 10;
  if (bookCount > 0 && x >= heroX && x < heroX + kHeroCoverWidth && y >= heroY && y < heroY + metrics.homeCoverHeight) {
    index = 0;
    return true;
  }

  // 2. Check Grid
  int gridStartY = heroY + metrics.homeCoverHeight + 40;
  int cellWidth = screenWidth / kGridCols;
  int cellHeight = (metrics.homeCoverTileHeight - (gridStartY - metrics.homeTopPadding)) / kGridRows;

  if (y >= gridStartY && y < gridStartY + kGridRows * cellHeight) {
    int col = x / cellWidth;
    int row = (y - gridStartY) / cellHeight;
    if (col >= 0 && col < kGridCols && row >= 0 && row < kGridRows) {
        int idx = 1 + col + row * kGridCols;
        if (idx < bookCount) {
            index = idx;
            return true;
        }
    }
  }

  // 3. Check Bottom Bar
  int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  if (y >= menuTop) {
      int renderedMenuCount = menuCount - bookCount;
      if (renderedMenuCount > 0) {
          int itemWidth = screenWidth / renderedMenuCount;
          int menuIdx = x / itemWidth;
          if (menuIdx >= 0 && menuIdx < renderedMenuCount) {
              index = bookCount + menuIdx;
              return true;
          }
      }
  }

  return false;
}
