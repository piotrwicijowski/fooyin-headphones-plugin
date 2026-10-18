// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/plugins/coreplugincontext.h>
#include <gui/plugins/guiplugincontext.h>

namespace Fooyin::Headphones::Tests {

// Inert host services: loading/initialising the scaffold must not dereference
// playback, decoder, settings or GUI dependencies, or construct audio resources.
inline CorePluginContext coreContext()
{
    return {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {}, {}, nullptr, {}};
}

inline GuiPluginContext guiContext()
{
    GuiPluginContext context {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    context.conversionService    = nullptr;
    context.outputProfileManager = nullptr;
    return context;
}

} // namespace Fooyin::Headphones::Tests
