#pragma once

#include "nested.h"

using Report = std::function<void(const std::wstring&)>;

void RunNestedWorkflow(std::stop_token stop, const Report& report, const NestedFrames& frames,
                       const Profile& profile, bool checkOnly, HANDLE job);
