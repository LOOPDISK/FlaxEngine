// mafi's splash screen

// Copyright (c) Wojciech Figat. All rights reserved.

#include "SplashScreen.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Types/TimeSpan.h"
#include "Engine/Engine/CommandLine.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Render2D/FontAsset.h"
#include "Engine/Render2D/Font.h"
#include "Engine/Render2D/TextLayoutOptions.h"
#include "Engine/Render2D/Render2D.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Content/Content.h"
#include "FlaxEngine.Gen.h"

// Randomly picked, limited to 50 characters width and 2 lines
const Char* SplashScreenQuotes[] =
{
    TEXT("[  S U B S T R A T A  ]"),
};


SplashScreen::~SplashScreen()
{
    // Ensure to be closed
    Close();
}

void SplashScreen::Show()
{
    // Skip if already shown or in headless mode
    if (IsVisible() || CommandLine::Options.Headless.IsTrue())
        return;

    LOG(Info, "Showing splash screen");

    // Create window
    const float dpiScale = Platform::GetDpiScale();
    CreateWindowSettings settings;
    settings.Title = TEXT("Flax Editor");
    settings.Size.X = 500 * dpiScale;
    settings.Size.Y = 170 * dpiScale;
    settings.HasBorder = false;
    settings.AllowInput = true;
    settings.AllowMinimize = false;
    settings.AllowMaximize = false;
    settings.AllowDragAndDrop = false;
    settings.IsTopmost = false;
    settings.IsRegularWindow = false;
    settings.HasSizingFrame = false;
    settings.ShowAfterFirstPaint = true;
    settings.StartPosition = WindowStartPosition::CenterScreen;
    _window = Platform::CreateWindow(settings);

    // Register window events
    _window->Closing.Bind([](ClosingReason reason, bool& cancel)
        {
            // Disable closing by user
            if (reason == ClosingReason::User)
                cancel = true;
        });
    _window->HitTest.Bind([](const Float2& mouse, WindowHitCodes& hit, bool& handled)
        {
            // Allow to drag window by clicking anywhere
            hit = WindowHitCodes::Caption;
            handled = true;
        });
    _window->Shown.Bind<SplashScreen, &SplashScreen::OnShown>(this);
    _window->Draw.Bind<SplashScreen, &SplashScreen::OnDraw>(this);

    // Setup
    _dpiScale = dpiScale;
    _size = settings.Size;
    _startTime = DateTime::NowUTC();
    auto str = Globals::ProjectFolder;
#if PLATFORM_WIN32
    str.Replace('/', '\\');
#else
    str.Replace('\\', '/');
#endif
    _infoText = String::Format(TEXT("Flax Editor {0}\n{1}\nProject: {2}"), TEXT(FLAXENGINE_VERSION_TEXT), TEXT(FLAXENGINE_COPYRIGHT), str);
    _quote = SplashScreenQuotes[rand() % ARRAY_COUNT(SplashScreenQuotes)];

    // Load font
    auto font = Content::LoadAsyncInternal<FontAsset>(TEXT("Editor/Fonts/Roboto-Regular"));
    if (font == nullptr)
    {
        LOG(Fatal, "Cannot load GUI primary font.");
    }
    else
    {
        if (font->IsLoaded())
            OnFontLoaded(font);
        else
            font->OnLoaded.Bind<SplashScreen, &SplashScreen::OnFontLoaded>(this);
    }

    // Load custom image
    _splashTexture.Loaded.Bind<SplashScreen, &SplashScreen::OnSplashLoaded>(this);
    String splashImagePath = Globals::ProjectContentFolder / TEXT("SplashImage.flax");
    if (FileSystem::FileExists(splashImagePath))
        _splashTexture = Content::LoadAsync<Texture>(splashImagePath);

    _window->Show();
}

void SplashScreen::Close()
{
    if (!IsVisible())
        return;

    LOG(Info, "Closing splash screen");

    // Close window
    _window->Close(ClosingReason::CloseEvent);
    _window = nullptr;

    _titleFont = nullptr;
    _subtitleFont = nullptr;
    _splashTexture = nullptr;
}

void SplashScreen::OnShown()
{
    // Focus on shown
    _window->Focus();
    _window->BringToFront(false);
}

void SplashScreen::OnDraw()
{
    const float s = _dpiScale;
    const float width = _size.X;
    const float height = _size.Y;

    // Peek time
    const float time = static_cast<float>((DateTime::NowUTC() - _startTime).GetTotalSeconds());

    // Background
    float lightBarHeight = 112 * s;
    if (_splashTexture != nullptr)
    {
        if (_splashTexture->IsLoaded())
        {
            lightBarHeight = height - lightBarHeight + 20 * s;
            Render2D::DrawTexture(_splashTexture, Rectangle(0, 0, width, height));
            Color rectColor = Color::FromRGB(0x0C0C0C);
            Render2D::FillRectangle(Rectangle(0, lightBarHeight, width, height - lightBarHeight), rectColor.AlphaMultiplied(0.85f), rectColor.AlphaMultiplied(0.85f), rectColor, rectColor);
        }
    }
    else
    {
        Render2D::FillRectangle(Rectangle(0, 0, width, 150 * s), Color::FromRGB(0x1C1C1C));
        Render2D::FillRectangle(Rectangle(0, lightBarHeight, width, height), Color::FromRGB(0x0C0C0C));
    }

    // Animated border
    const float anim = Math::Sin(time * 4.0f) * 0.5f + 0.5f;
    Render2D::DrawRectangle(Rectangle(0, 0, width, height), Math::Lerp(Color::Gray * 0.8f, Color::FromRGB(0x007ACC), anim));

    // Check fonts
    if (!HasLoadedFonts())
        return;

    // Title
    const auto titleLength = _titleFont->MeasureText(GetTitle());
    TextLayoutOptions layout;
    layout.Bounds = Rectangle(10 * s, 10 * s, width - 10 * s, 50 * s);
    layout.HorizontalAlignment = TextAlignment::Near;
    layout.VerticalAlignment = TextAlignment::Near;
    layout.Scale = Math::Min((width - 20 * s) / titleLength.X, 1.0f);
    Render2D::DrawText(_titleFont, GetTitle(), Color::White, layout);

    // Subtitle
    String subtitle(_quote);
    //if (!subtitle.EndsWith(TEXT('!')) && !subtitle.EndsWith(TEXT('?')))
    //{
    //    for (int32 i = static_cast<int32>(time * 2.0f) % 4; i > 0; i--)
    //        subtitle += TEXT('.');
    //    for (int32 i = 0; i < 4 - static_cast<int32>(time * 2.0f) % 4; i++)
    //        subtitle += TEXT(' ');
    //}
    int index = 1 + (subtitle.Length() - 2) * anim;
    if (subtitle[index] == ' ')
    {
        subtitle[index] = '-';
    }

    if (_splashTexture != nullptr)
    {
        layout.Bounds = Rectangle(width - 224 * s, lightBarHeight + 4 * s, 220 * s, 35 * s);
        layout.VerticalAlignment = TextAlignment::Near;
    }
    else
    {
        layout.Bounds = Rectangle(width - 224 * s, lightBarHeight - 39 * s, 220 * s, 35 * s);
        layout.VerticalAlignment = TextAlignment::Far;
    }
    layout.Scale = 1.0f;
    layout.HorizontalAlignment = TextAlignment::Far;
    Render2D::DrawText(_subtitleFont, subtitle, Color::FromRGB(0x8C8C8C), layout);

    // Additional info
    const float infoMargin = 6 * s;
    if (_splashTexture != nullptr)
        layout.Bounds = Rectangle(infoMargin + 4 * s, lightBarHeight + infoMargin, width - (2 * infoMargin), height - lightBarHeight - (2 * infoMargin));
    else
        layout.Bounds = Rectangle(infoMargin, lightBarHeight + infoMargin, width - (2 * infoMargin), height - lightBarHeight - (2 * infoMargin));

    layout.HorizontalAlignment = TextAlignment::Near;
    layout.VerticalAlignment = TextAlignment::Center;
    Render2D::DrawText(_subtitleFont, _infoText, Color::FromRGB(0xFFFFFF) * 0.9f, layout);
}

bool SplashScreen::HasLoadedFonts() const
{
    return _titleFont && _subtitleFont;
}

void SplashScreen::OnFontLoaded(Asset* asset)
{
    ASSERT(asset && asset->IsLoaded());
    auto font = (FontAsset*)asset;

    font->OnLoaded.Unbind<SplashScreen, &SplashScreen::OnFontLoaded>(this);

    // Create fonts
    const float s = _dpiScale;
    _titleFont = font->CreateFont(35 * s);
    _subtitleFont = font->CreateFont(9 * s);
}

void SplashScreen::OnSplashLoaded()
{
    // Resize window to be larger if texture is being used
    auto desktopSize = Platform::GetDesktopSize();
    auto xSize = (desktopSize.X / (600.0f * 3.0f)) * 600.0f;
    auto ySize = (desktopSize.Y / (200.0f * 3.0f)) * 200.0f;
    _window->SetClientSize(Float2(xSize, ySize));
    _size = _window->GetSize();
    _window->SetPosition((desktopSize - _size) * 0.5f);
}
