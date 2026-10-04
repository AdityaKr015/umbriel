#include "view/maximize.h"

#include "check.h"

using umbriel::maximizeRequestTargetsEdges;

UMBRIEL_TEST(freshClientMaximizeTargetsColumn) { CHECK(!maximizeRequestTargetsEdges(false, false)); }

UMBRIEL_TEST(clientCanLeaveEdgesMaximize) { CHECK(maximizeRequestTargetsEdges(true, false)); }

UMBRIEL_TEST(configuredPolicySendsFreshRequestToEdges) { CHECK(maximizeRequestTargetsEdges(false, true)); }

UMBRIEL_TEST(configuredPolicyKeepsEdgesRequestsonEdges) { CHECK(maximizeRequestTargetsEdges(true, true)); }

int main() { return RUN_TESTS(); }
