// QR codes as vector geometry (crisp at any size and DPI).
#pragma once

#include <string>

#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace mdui
{
    // The QR code of `text` (error correction M), one unit per module, without the quiet zone:
    // show it in a Path with Stretch="Uniform" on a light background with a margin of four
    // modules. Null when the text does not fit in a QR code.
    winrt::Microsoft::UI::Xaml::Media::Geometry QrGeometry(std::string const& text);
}
