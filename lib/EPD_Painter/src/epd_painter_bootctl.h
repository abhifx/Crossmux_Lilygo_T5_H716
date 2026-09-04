#pragma once

#include "build_opt.h"
#include "EPD_Painter.h"

// =============================================================================
// EPD_BootCtl — reset-toggle power management with shutdown image support.
//
// Every reset acts as a power toggle:
//   flag=false (device was running)  → shutdownPending() returns true.
//   flag=true  (device was sleeping) → DC-balance unpaint, shutdownPending() false.
//
// Usage — simple (built-in fractal, no confirmation):
//   EPD_BootCtl boot(epd);
//   if (boot.shutdownPending()) boot.shutdown();
//
// Usage — custom image via IImageProvider:
//   class MyImage : public EPD_BootCtl::IImageProvider {
//   public:
//       uint8_t* getBootImage(uint16_t w, uint16_t h) override { ... }
//   };
//   MyImage img;
//   EPD_BootCtl boot(epd, img);
//   if (boot.shutdownPending()) boot.shutdown();
//
// The provider is re-constructed on every boot, so nothing needs to survive
// a reset — NVS only stores the running/sleeping flag.
//
// Board-specific power-off:
//   pin_syspwr >= 0  (M5PaperS3)    — HIGH→LOW pulse on that GPIO.
//   pin_syspwr == -1 (LilyGo T5 S3) — BQ25896 BATFET disable over I2C.
// =============================================================================

class EPD_BootCtl {
public:

    // -------------------------------------------------------------------------
    // Implement this interface to supply a shutdown image.
    // getBootImage() is called on shutdown (to paint) and on startup (to unpaint
    // the same image for DC balance).
    // Return a PSRAM-allocated packed 2bpp buffer; EPD_BootCtl frees it.
    // -------------------------------------------------------------------------
    class IImageProvider {
    public:
        virtual uint8_t* getBootImage(uint16_t w, uint16_t h) = 0;
        virtual ~IImageProvider() = default;
    };

    // -------------------------------------------------------------------------
    // Built-in XOR fractal image — used by default, also available explicitly.
    // Each grey level covers ~25% of pixels for optimal DC balance.
    // -------------------------------------------------------------------------
    class FractalImage : public IImageProvider {
    public:
        uint8_t* getBootImage(uint16_t w, uint16_t h) override;
    };

    static FractalImage fractal;  // shared default instance

    // No provider — uses EPD_BootCtl::fractal.
    explicit EPD_BootCtl(EPD_Painter& epd);

    // With provider — calls provider.getBootImage() on shutdown and startup.
    EPD_BootCtl(EPD_Painter& epd, IImageProvider& provider);

    // True when a shutdown was requested (reset pressed while running).
    bool shutdownPending() const { return _pending; }

    // Dismiss the pending shutdown — on next reset it will be pending again.
    void cancelShutdown() { _pending = false; }

    // Paint the image, cut power, deep sleep.  Never returns.
    [[noreturn]] void shutdown();

private:
    EPD_Painter&    _epd;
    IImageProvider& _provider;
    bool            _pending = false;

    static constexpr const char* NVS_NS       = "epdboot";
    static constexpr const char* NVS_KEY_FLAG = "flag";

    void     _init();
    uint8_t* _getImage() const;
    void     _paintAndPowerOff();

    bool _isUsbConnected() const;
    [[noreturn]] void _powerOff();

    bool _readFlag()          const;
    void _writeFlag(bool val) const;
};
