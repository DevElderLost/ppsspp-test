// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

// Fill screen: the game's picture covers the whole display (a 21:9 phone, say) without being stretched.
// Ported from the "Fill" aspect of the Yakumo (MHP3rd) host.
//
//  * The game's 3D view is given the shape of the display. Monster Hunter Portable 3rd keeps its projection's
//    aspect ratio in its camera object, starting from the constant 480/272; with a wider aspect the game keeps its
//    vertical field of view and widens the horizontal one. It still draws into its 480x272 screen, which is then
//    spread over the display, so the widened view comes out with square pixels. The game's code is checked word for
//    word before anything is written (see kSignature in FillScreen.cpp), so another game, or another version,
//    is never touched.
//  * The 2D interface (HUD, menus) would be stretched by the same spreading. Its through-mode draws into the
//    displayed framebuffer are pulled in about the screen's centre to square pixels again. Draws that cover the
//    screen's width (fades, backdrops) and draws that sample a rendered picture (blur, quest backgrounds) belong
//    with the 3D view and stay spread.
//  * A movie has the PSP's shape: while one plays, the game gets its own aspect ratio back and the display
//    keeps the PSP's shape with bars beside it.

namespace FillScreen {

// Forget what was learned about the running game. Call when a game starts or the GPU is reinitialized.
void Reset();

// Once per game frame, on the emulation thread, when the frame is about to be presented: gives the game the
// display's aspect ratio when Fill screen is on (and puts the game's own back when it is not).
void BeginFrame();

// True while the game's view has been widened to the display's shape. Then the presentation stretches the
// picture over the whole display (CalculateDisplayOutputRect), and the 2D interface is fitted. Any thread.
bool Active();

// Set before each draw: whether it goes into a framebuffer the game has displayed (as opposed to an offscreen one).
void SetTargetDisplayed(bool displayed);

// A through-mode draw sampled a video frame (a movie is playing).
void NoteVideoDraw();

// How much to shrink the 2D interface about the centre of the 480x272 screen to make its pixels square again.
// Returns false when it should be left as it is.
bool GetInterfaceFit(float *fitX, float *fitY);

// A through-mode draw at least this wide (in PSP pixels) covers the screen and is not fitted.
constexpr float kScreenWideDraw = 470.0f;

}  // namespace FillScreen
