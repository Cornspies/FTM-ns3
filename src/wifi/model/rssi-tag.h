/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RSSI_TAG_H
#define RSSI_TAG_H

#include "wifi-units.h"

#include "ns3/tag.h"
#include "ns3/wifi-export.h"

namespace ns3
{

/**
 * @ingroup wifi
 *
 * Tag carrying the received power of a frame, so that the MAC can store it together with the
 * other information about an AP found through scanning. Unlike the SNR carried by the SnrTag,
 * it does not depend on the interference during the reception of the frame.
 */
class WIFI_EXPORT RssiTag : public Tag
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;

    /**
     * Set the RSSI to the given value.
     *
     * @param rssi the RSSI
     */
    void Set(dBm_u rssi);

    /**
     * Return the RSSI value.
     *
     * @return the RSSI
     */
    dBm_u Get() const;

  private:
    dBm_u m_rssi{0}; ///< RSSI
};

} // namespace ns3

#endif /* RSSI_TAG_H */
