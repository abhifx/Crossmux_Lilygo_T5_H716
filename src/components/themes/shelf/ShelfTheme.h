#pragma once

#include "components/themes/lyra/LyraTheme.h"

namespace ShelfMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics metrics = LyraMetrics::values;
  metrics.homeTopPadding = 50;
  metrics.homeCoverHeight = 350;
  metrics.homeCoverTileHeight = 780;
  metrics.homeRecentBooksCount = 11;
  metrics.homeContinueReadingInMenu = false;
  metrics.homeShowRecentBookTitle = false;
  metrics.homeMenuTopOffset = 0;
  metrics.menuRowHeight = 90;
  metrics.tabBarHeight = 0; // No tabs
  return metrics;
}
inline constexpr ThemeMetrics values = makeValues();
}  // namespace ShelfMetrics

class ShelfTheme final : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const override;
  void drawHomeMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                    const std::function<std::string(int index)>& buttonLabel,
                    const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  bool homeIndexFromPoint(const GfxRenderer& renderer, int x, int y, int bookCount, int menuCount,
                          int& index) const override;
};
