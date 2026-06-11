// nhllegacy custom renderer — graphics system
//
// NhlD3D12GraphicsSystem subclasses the SDK's concrete D3D12 graphics system so
// we can substitute our own CommandProcessor. This is the Sprint-1 injection
// seam from the renderer plan: the SDK builds the guest GPU front-end (ring
// buffer, PM4 parser, RegisterFile, MMIO trap) for us, and CreateCommandProcessor
// is the single protected hook that decides which backend receives the decoded
// IssueDraw / IssueCopy / LoadShader / IssueSwap callbacks.
//
// At this stage NhlD3D12CommandProcessor only logs-and-delegates to the base
// D3D12 implementation, so behavior is identical to the stock backend. The build
// itself is the experiment: it tells us whether the concrete D3D12CommandProcessor
// constructor + protected virtuals are exported from rexruntime.dll (i.e. whether
// "subclass the concrete backend" is viable, vs. having to subclass the abstract
// GraphicsSystem base — open question 1 in the plan).

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>

namespace nhl::graphics {

class NhlD3D12GraphicsSystem : public rex::graphics::d3d12::D3D12GraphicsSystem {
 public:
  NhlD3D12GraphicsSystem() = default;
  ~NhlD3D12GraphicsSystem() override = default;

  std::string name() const override;

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace nhl::graphics
