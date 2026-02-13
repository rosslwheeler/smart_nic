#pragma once

#include <cstdint>
#include <vector>

namespace nic {

/// Standard PCIe capability IDs.
enum class CapabilityId : std::uint8_t {
  PowerManagement = 0x01,
  AGP = 0x02,
  VPD = 0x03,
  SlotId = 0x04,
  MSI = 0x05,
  CompactPCIHotSwap = 0x06,
  PCIX = 0x07,
  HyperTransport = 0x08,
  VendorSpecific = 0x09,
  DebugPort = 0x0A,
  CompactPCICRC = 0x0B,
  PCIHotPlug = 0x0C,
  PCIBridgeSubsystemVID = 0x0D,
  AGP8x = 0x0E,
  SecureDevice = 0x0F,
  PCIExpress = 0x10,
  MSIX = 0x11,
  SATA = 0x12,
  AdvancedFeatures = 0x13,
  EnhancedAllocation = 0x14,
  FlatteningPortalBridge = 0x15,
};

/// PCIe Extended Capability IDs (in extended config space, offset 0x100+).
enum class ExtCapabilityId : std::uint16_t {
  AdvancedErrorReporting = 0x0001,
  VirtualChannel = 0x0002,
  DeviceSerialNumber = 0x0003,
  PowerBudgeting = 0x0004,
  RootComplexLinkDecl = 0x0005,
  RootComplexInternalLink = 0x0006,
  RootComplexEventCollector = 0x0007,
  MFVC = 0x0008,
  VirtualChannelMFVC = 0x0009,
  RCRB = 0x000A,
  VendorSpecific = 0x000B,
  ConfigAccessCorrelation = 0x000C,
  ACS = 0x000D,
  ARI = 0x000E,
  ATS = 0x000F,
  SRIOV = 0x0010,
  MRIOV = 0x0011,
  Multicast = 0x0012,
  PageRequestInterface = 0x0013,
  ResizableBAR = 0x0015,
  DynamicPowerAllocation = 0x0016,
  TPHRequester = 0x0017,
  LatencyToleranceReporting = 0x0018,
  SecondaryPCIExpress = 0x0019,
  PMUX = 0x001A,
  PASID = 0x001B,
  LNR = 0x001C,
  DPC = 0x001D,
  L1PMSubstates = 0x001E,
  PrecisionTimeMeasurement = 0x001F,
  MPCIe = 0x0020,
  FRSQueueing = 0x0021,
  ReadinessTimeReporting = 0x0022,
  DesignatedVendorSpecific = 0x0023,
  VFResizableBAR = 0x0024,
  DataLinkFeature = 0x0025,
  PhysicalLayer16GT = 0x0026,
  LaneMarginReceiver = 0x0027,
  HierarchyId = 0x0028,
  NPEM = 0x0029,
  PhysicalLayer32GT = 0x002A,
  AlternateProtocol = 0x002B,
  SystemFirmwareIntermediary = 0x002C,
};

/// Standard capability entry (in legacy config space 0x00-0xFF).
struct Capability {
  CapabilityId id;
  std::uint8_t offset;   ///< Offset in config space
  std::uint8_t next;     ///< Offset of next capability (0 = end)
  std::uint16_t length;  ///< Total length of this capability structure
};

/// Extended capability entry (in extended config space 0x100-0xFFF).
struct ExtCapability {
  ExtCapabilityId id;
  std::uint16_t version;
  std::uint16_t offset;  ///< Offset in config space
  std::uint16_t next;    ///< Offset of next capability (0 = end)
  std::uint16_t length;  ///< Total length of this capability structure
};

/// Collection of capabilities for a device.
struct CapabilityList {
  std::vector<Capability> standard;
  std::vector<ExtCapability> extended;

  [[nodiscard]] std::uint8_t first_capability_offset() const noexcept {
    if (standard.empty()) {
      return 0;
    }
    return standard[0].offset;
  }
};

/// Create default capability list for a typical NIC.
/// Includes: PCIe, MSI-X, Power Management, and common extended caps.
inline CapabilityList MakeDefaultCapabilities() {
  CapabilityList caps;

  // Standard capabilities (linked list in legacy config space)
  // Layout: PM @ 0x40 -> MSI-X @ 0x50 -> PCIe @ 0x70

  caps.standard.push_back(Capability{
      .id = CapabilityId::PowerManagement,
      .offset = 0x40,
      .next = 0x50,
      .length = 8,
  });

  caps.standard.push_back(Capability{
      .id = CapabilityId::MSIX,
      .offset = 0x50,
      .next = 0x70,
      .length = 12,
  });

  caps.standard.push_back(Capability{
      .id = CapabilityId::PCIExpress,
      .offset = 0x70,
      .next = 0x00,  // End of list
      .length = 60,  // PCIe cap is 60 bytes for endpoint
  });

  // Extended capabilities (linked list starting at 0x100)
  // Layout: AER @ 0x100 -> SR-IOV @ 0x150 -> ARI @ 0x200

  caps.extended.push_back(ExtCapability{
      .id = ExtCapabilityId::AdvancedErrorReporting,
      .version = 2,
      .offset = 0x100,
      .next = 0x150,
      .length = 48,
  });

  caps.extended.push_back(ExtCapability{
      .id = ExtCapabilityId::SRIOV,
      .version = 1,
      .offset = 0x150,
      .next = 0x200,
      .length = 64,
  });

  caps.extended.push_back(ExtCapability{
      .id = ExtCapabilityId::ARI,
      .version = 1,
      .offset = 0x200,
      .next = 0x000,  // End of list
      .length = 8,
  });

  return caps;
}

}  // namespace nic
