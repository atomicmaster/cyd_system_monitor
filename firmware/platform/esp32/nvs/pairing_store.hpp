// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware/domain/host_link.hpp"

namespace firmware::platform::esp32::nvs {

// One-to-one Host pairing persisted in NVS. The device identity is generated
// once (hardware RNG) on first use and never changes; the paired Host is
// stored only after touch confirmation. Identities are confirmation data,
// not secrets.
class NvsPairingStore : public firmware::domain::PairingStore {
 public:
  firmware::domain::Identity DeviceIdentity() override;
  std::optional<firmware::domain::Identity> PairedHost() override;
  bool SavePairedHost(const firmware::domain::Identity& host) override;
  bool ClearPairedHost() override;
};

}  // namespace firmware::platform::esp32::nvs
