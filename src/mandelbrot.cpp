#include "mandelbrot.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <sstream>

#include "cyclic_gradient.h"
#include "open_sans_semibold.h"
#include "vector_util.h"

// For use by our constructor.
// Returns the appropriate initial world bounds.
// Should always contain the coordinates bounded in (-2.5, -1.5) to (0.5, 1.5).
std::pair<sf::Vector2<double>, sf::Vector2<double>>
MandelbrotViewer::getInitialWorldBoundsForWindowSize(unsigned int windowWidth,
                                                     unsigned int windowHeight) {
    assert(windowWidth > 0);
    assert(windowHeight > 0);
    const double aspectRatio = static_cast<double>(windowWidth) / windowHeight;
    const sf::Vector2<double> worldCenter{-1., 0.};
    sf::Vector2<double> initWorldSize = {3., 3.};
    if (windowWidth >= windowHeight) {
        // world height will be 3
        initWorldSize.x = 3 * aspectRatio;
    } else {
        // world width will be 3
        initWorldSize.y = 3 / aspectRatio;
    }
    return {worldCenter - initWorldSize / 2., worldCenter + initWorldSize / 2.};
}

MandelbrotViewer::MandelbrotViewer(unsigned int windowWidth, unsigned int windowHeight)
    : mWindow(sf::VideoMode(sf::Vector2u{windowWidth, windowHeight}), "Mandelbrot set viewer"),
      mWindowSize(windowWidth, windowHeight),
      mFont(EmbeddedFonts::OpenSans_SemiBold_ttf, EmbeddedFonts::OpenSans_SemiBold_ttf_len),
      mCursorWorldPosText(mFont),
      mCursorWorldPosTextShadow(mFont),
      mViewBuffer(sf::Vector2u{windowWidth, windowHeight}, sf::Color::Blue),
      mViewBufferGPU(mViewBuffer),
      mViewSprite(mViewBufferGPU) {
    assert(windowWidth > 0);
    assert(windowHeight > 0);
    std::tie(mMinPointWorld, mMaxPointWorld) =
        getInitialWorldBoundsForWindowSize(windowWidth, windowHeight);

    // disable key repeat
    mWindow.setKeyRepeatEnabled(false);
    mWindow.setFramerateLimit(60);

    // Initially no text to show
    mCursorWorldPosText.setString("");
    mCursorWorldPosTextShadow.setString("");
    // Character size of 14 pixels
    mCursorWorldPosText.setCharacterSize(14);
    mCursorWorldPosTextShadow.setCharacterSize(14);
    // Text is white, shadow is black.
    mCursorWorldPosText.setFillColor(sf::Color::White);
    mCursorWorldPosTextShadow.setFillColor(sf::Color(0, 0, 0, 130));
    // Set text position
    mCursorWorldPosText.setPosition({10, 10});
    mCursorWorldPosTextShadow.setPosition({10, 10});
    // Give text shadow extra thickness.
    mCursorWorldPosTextShadow.setOutlineColor(sf::Color(0, 0, 0, 130));
    mCursorWorldPosTextShadow.setOutlineThickness(3);
}

void MandelbrotViewer::run() {
    sf::Clock clock;
    sf::Time lastTime = sf::Time();
    int maxIters = MAX_ITERS_LOWER_BOUND;
    int renderedMaxIters = -1;
    while (true) {
        sf::Time curTime = clock.getElapsedTime();
        // Handle inputs
        InputSummary inputSummary = readInputs();
        if (inputSummary.shouldClose) {
            mWindow.close();
            return;
        }
        // Determine whether any of the inputs cause the view to change.
        bool viewChanged = (inputSummary.zoomDistance != 0) ||
                           (inputSummary.panVector != sf::Vector2<double>{0., 0.}) ||
                           inputSummary.shouldResize;
        // Update
        updateViewState(inputSummary, curTime - lastTime);
        updateUIText(inputSummary.mousePosition);
        maxIters = (viewChanged ? MAX_ITERS_LOWER_BOUND
                                : std::min(maxIters * ITERS_MULTIPLIER, MAX_ITERS_UPPER_BOUND));
        // Render:
        if (viewChanged || maxIters != renderedMaxIters) {
            drawIntoViewBuffer(maxIters);
            copyViewBufferToGPU();
            renderedMaxIters = maxIters;
        }
        draw();
        // update timing stats
        lastTime = curTime;
    }
}

MandelbrotViewer::InputSummary MandelbrotViewer::readInputs() {
    InputSummary ret{};
    while (const std::optional<sf::Event> event = mWindow.pollEvent()) {
        if (event->is<sf::Event::Closed>()) {
            ret.shouldClose = true;
            return ret;  // just stop early
        } else if (const auto* mouseScrolled = event->getIf<sf::Event::MouseWheelScrolled>()) {
            // If multiple of these, it adds up.
            ret.zoomDistance += mouseScrolled->delta;
        } else if (const auto* resized = event->getIf<sf::Event::Resized>()) {
            ret.desiredWindowSize = resized->size;
            ret.shouldResize = true;
        }
    }
    // pan is a continuous action, so we'll just check current state for each key. Note, NOT
    // else if.
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::W)) {
        ret.panVector += {0., 1};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::S)) {
        ret.panVector += {0., -1.};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::A)) {
        ret.panVector += {-1., 0.};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::D)) {
        ret.panVector += {1., 0.};
    }
    ret.mousePosition = sf::Mouse::getPosition(mWindow);
    return ret;
}

void MandelbrotViewer::updateViewState(const InputSummary& inputs, sf::Time deltaTime) {
    if (inputs.shouldResize) {
        handleWindowResize(inputs.desiredWindowSize);
    }
    if (inputs.zoomDistance != 0) {
        handleZoom(inputs.zoomDistance, inputs.mousePosition);
    }
    if (inputs.panVector != sf::Vector2<double>{0., 0.}) {
        handlePans(inputs.panVector, deltaTime);
    }
}

void MandelbrotViewer::handleZoom(double scrollDistance, sf::Vector2i mousePosition) {
    double worldViewFactor = std::pow(ZOOM_EXPONENT_BASE, scrollDistance);

    // The world point under the cursor stays fixed during the zoom
    const sf::Vector2<double> anchor = windowPosToWorld(sf::Vector2<double>(mousePosition));

    // cursor position in world coordinates stays fixed, so we scale the world rectangle around that
    // point
    mMinPointWorld = anchor + (mMinPointWorld - anchor) * worldViewFactor;
    mMaxPointWorld = anchor + (mMaxPointWorld - anchor) * worldViewFactor;
}

void MandelbrotViewer::handleWindowResize(sf::Vector2u newSize)  // newSize is in window coords.
{
    mWindow.setView(sf::View(
        sf::FloatRect({0, 0}, {static_cast<float>(newSize.x), static_cast<float>(newSize.y)})));

    // avoid divide by zero and other weirdnesses if the window is resized to 0 in either dimension
    if (newSize.x == 0 || newSize.y == 0) {
        return;
    }

    const sf::Vector2<double> oldWin(mWindowSize);
    const sf::Vector2<double> newWin(newSize);
    // Keep the same world center
    const sf::Vector2<double> center = (mMinPointWorld + mMaxPointWorld) / 2.0;

    // Scale the world rectangle by the same factor as the window was scaled in each dimension
    const sf::Vector2<double> oldWorldSize = mMaxPointWorld - mMinPointWorld;
    const sf::Vector2<double> newWorldSize = oldWorldSize * (newWin / oldWin);
    mMinPointWorld = center - newWorldSize / 2.0;
    mMaxPointWorld = center + newWorldSize / 2.0;

    // update CPU-side image buffer size to have enough memory for all the pixels:
    mViewBuffer.resize(newSize);

    (void)mViewBufferGPU.resize(newSize);

    // The sprite will have an incorrect view into the texture after resize, so we update:
    mViewSprite.setTextureRect(sf::IntRect({0, 0}, sf::Vector2i(newSize)));
    mWindowSize = newSize;  // update mWindowSize.
}

void MandelbrotViewer::handlePans(sf::Vector2<double> basePanVector, sf::Time deltaTime) {
    const sf::Vector2<double> worldViewSize = mMaxPointWorld - mMinPointWorld;
    const double panScale = (worldViewSize.x + worldViewSize.y) * 0.5;
    const auto panDelta =
        basePanVector * static_cast<double>(deltaTime.asMilliseconds()) * panScale * PAN_FACTOR;
    mMinPointWorld += panDelta;
    mMaxPointWorld += panDelta;
}

std::string MandelbrotViewer::formatCoords(const sf::Vector2<double>& pos) {
    std::stringstream ss;
    ss << std::setprecision(15) << "(" << pos.x << ", " << pos.y << ")";
    return ss.str();
}

// updateUIText updates mCursorWorldPosText and mCursorWorldPosTextShadow
// to show the cursor's current **world coordinates** position
void MandelbrotViewer::updateUIText(sf::Vector2i mouseWindowCoords) {
    auto coordsString = formatCoords(windowPosToWorld(sf::Vector2<double>(mouseWindowCoords)));
    mCursorWorldPosText.setString(coordsString);
    mCursorWorldPosTextShadow.setString(coordsString);
}

double MandelbrotViewer::mandelbrot(double cX, double cY, int maxIters) const {
    // z starts at 0 + 0i
    double zX = 0.0;
    double zY = 0.0;
    // n counts which iteration we're on, starting at 1
    for (int n = 1; n <= maxIters; ++n) {
        // z' = z^2 + c, using (a + bi)^2 = (a^2 - b^2) + (2ab)i
        double zPrimeX = zX * zX - zY * zY + cX;
        double zPrimeY = 2.0 * zX * zY + cY;
        // |z'| > 2 is the same as |z'|^2 > 4, which avoids a sqrt on every iteration
        if (zPrimeX * zPrimeX + zPrimeY * zPrimeY > 4.0) {
            return static_cast<double>(n);
        }
        // still bounded, so continue from z'
        zX = zPrimeX;
        zY = zPrimeY;
    }
    return std::numeric_limits<double>::infinity();
}

double MandelbrotViewer::mandelbrotSmooth(double cX, double cY, int maxIters) const {
    constexpr double ESCAPE_RADIUS_SQUARED = 16.0;
    double zX = 0.0;
    double zY = 0.0;
    for (int n = 1; n <= maxIters; ++n) {
        double zPrimeX = zX * zX - zY * zY + cX;
        double zPrimeY = 2.0 * zX * zY + cY;
        double modulusSq = zPrimeX * zPrimeX + zPrimeY * zPrimeY;
        if (modulusSq > ESCAPE_RADIUS_SQUARED) {
            // Fractional escape time: n + 1 - ln(ln|z_n|) / ln 2
            double modulus = std::sqrt(modulusSq);
            return n + 1.0 - std::log(std::log(modulus)) / LOG_2;
        }
        zX = zPrimeX;
        zY = zPrimeY;
    }

    return std::numeric_limits<double>::infinity();
}

// windowPosToWorld takes a point in window coordinates and converts it to world coordinates
sf::Vector2<double> MandelbrotViewer::windowPosToWorld(const sf::Vector2<double>& pWindow) {
    // Size of the visible world rectangle
    const sf::Vector2<double> worldSize = mMaxPointWorld - mMinPointWorld;

    const double winW = static_cast<double>(mWindowSize.x);
    const double winH = static_cast<double>(mWindowSize.y);
    // Fraction of the way across the window
    const double tX = pWindow.x / winW;
    const double tY = pWindow.y / winH;
    // x: window and world both increase to the right, so start from the min x
    // y: window y points down but world y points up, so the top of the window
    // maps to the max world y, and we subtract as we go down.
    return {mMinPointWorld.x + tX * worldSize.x, mMaxPointWorld.y - tY * worldSize.y};
}

// drawIntoBuffer renders the current world view (bounded by mMinPointWorld and mMaxPointWorld)
// into mViewBuffer
void MandelbrotViewer::drawIntoViewBuffer(int maxIters) {
    const sf::Vector2u size = mViewBuffer.getSize();
    // For each pixel in the window, we will compute the corresponding world coordinate, and then
    // compute the mandelbrot value for that world coordinate, and then set the pixel in the buffer
    // to the corresponding color
    for (unsigned int py = 0; py < size.y; ++py) {
        for (unsigned int px = 0; px < size.x; ++px) {
            const sf::Vector2<double> pixelCenter{px + 0.5, py + 0.5};
            const sf::Vector2<double> world = windowPosToWorld(pixelCenter);
            const double n = mandelbrotSmooth(world.x, world.y, maxIters);
            // black if it diverged after maxIters, otherwise use the default cyclic gradient to
            // color it
            const sf::Color color =
                std::isinf(n) ? sf::Color::Black : CyclicGradient::DEFAULT_GRADIENT(n);

            mViewBuffer.setPixel({px, py}, color);
        }
    }
}

// copyViewBufferToGPU takes the drawn CPU-side buffer mViewBuffer and copies it to the
// GPU-side.
void MandelbrotViewer::copyViewBufferToGPU() {
    // send the CPU-side buffer to the GPU-side texture, so that it can be drawn to the window
    mViewBufferGPU.update(mViewBuffer);
}

// draw clears the window, draws the view, as well as the text with its shadow underneath
// it. Finally, the window is displayed.
void MandelbrotViewer::draw() {
    mWindow.clear();
    mWindow.draw(mViewSprite);
    mWindow.draw(mCursorWorldPosTextShadow);
    mWindow.draw(mCursorWorldPosText);
    mWindow.display();
}
