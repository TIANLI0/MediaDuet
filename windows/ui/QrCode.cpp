#include "pch.h"
#include "QrCode.h"

#include "qrcodegen/qrcodegen.hpp"

using namespace winrt::Microsoft::UI::Xaml::Media;

namespace mdui
{
    Geometry QrGeometry(std::string const& text)
    {
        try
        {
            auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            GeometryGroup group;
            group.FillRule(FillRule::Nonzero);
            int n = qr.getSize();
            // One rectangle per horizontal run of dark modules.
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n;)
                {
                    if (!qr.getModule(x, y))
                    {
                        ++x;
                        continue;
                    }
                    int start = x;
                    while (x < n && qr.getModule(x, y)) ++x;
                    RectangleGeometry r;
                    r.Rect({ static_cast<float>(start), static_cast<float>(y), static_cast<float>(x - start), 1.0f });
                    group.Children().Append(r);
                }
            return group;
        }
        catch (...)
        {
            return nullptr;
        }
    }
}
