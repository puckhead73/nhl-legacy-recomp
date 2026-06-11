#include "renderer/core/nhl_graphics_system.h"

#include <rex/ui/renderdoc_api.h>  // complete RenderDocAPI for the CP's unique_ptr member dtor

#include "renderer/core/nhl_command_processor.h"

namespace nhl::graphics {

std::string NhlD3D12GraphicsSystem::name() const {
  // Surfaced in the window title / logs so it's obvious our backend is live.
  return "nhl-d3d12 (rexglue front-end + log-and-delegate)";
}

std::unique_ptr<rex::graphics::CommandProcessor>
NhlD3D12GraphicsSystem::CreateCommandProcessor() {
  // `this` is the D3D12GraphicsSystem the base ctor expects; kernel_state() is a
  // public accessor on the GraphicsSystem base, populated by SetupGuestGpu before
  // the command processor is created.
  return std::make_unique<NhlD3D12CommandProcessor>(this, kernel_state());
}

}  // namespace nhl::graphics
