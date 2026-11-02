#pragma once

namespace apps {
namespace liedetect {

// Lie Detector — a party prank. The subject holds ENTER while answering; a
// polygraph trace driven by the mic level scrolls across the screen, a fake
// "analysis" runs, and a verdict is shown with a sound.
//
// The verdict is not science. The operator steers it secretly: press `z`
// (truth) or `x` (lie) any time before the verdict — the only feedback is a
// single dim pixel in the bottom-right corner. Without a hint the result is
// random, nudged by how loud/shaky the answer was.
void enter();
void tick();

}  // namespace liedetect
}  // namespace apps
