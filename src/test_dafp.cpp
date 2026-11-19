#include "DafpDiagnostics.h"
#include <cassert>

int main() {
    assert(DafpSwapchainMajor("AMD SwapChain 4.0.0") == 4);
    assert(DafpSwapchainMajor("3.1.7") == 3);
    assert(DafpSwapchainMajor("not available") == 0);
    assert(DafpSwapchainMajor("FSR4") == 0);

    DafpDiagnostics d{};
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::Inactive);
    d.nativeSwapchain = d.presentRecent = d.generatedRecent = true;
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::Unknown);
    d.swapchainMajor = 4;
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::PossibleUnverified);
    assert(EvaluateDafpEligibility(d, -1) == DafpEligibility::Unknown);
    d.syncInterval = 1;
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::Blocked);
    d.syncInterval = 0;
    assert(EvaluateDafpEligibility(d, false) == DafpEligibility::Blocked);
    d.swapchainMajor = 3;
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::Blocked);
    d.swapchainMajor = 4;
    d.generatedRecent = false;
    assert(EvaluateDafpEligibility(d, true) == DafpEligibility::Inactive);
}
