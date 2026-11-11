/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rssi-tag.h"

#include "ns3/double.h"

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(RssiTag);

TypeId
RssiTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RssiTag")
                            .SetParent<Tag>()
                            .SetGroupName("Wifi")
                            .AddConstructor<RssiTag>()
                            .AddAttribute("Rssi",
                                          "The received power (dBm) of the last packet received",
                                          DoubleValue(0.0),
                                          MakeDoubleAccessor(&RssiTag::Get),
                                          MakeDoubleChecker<double>());
    return tid;
}

TypeId
RssiTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
RssiTag::GetSerializedSize() const
{
    return sizeof(double);
}

void
RssiTag::Serialize(TagBuffer i) const
{
    i.WriteDouble(m_rssi);
}

void
RssiTag::Deserialize(TagBuffer i)
{
    m_rssi = i.ReadDouble();
}

void
RssiTag::Print(std::ostream& os) const
{
    os << "Rssi=" << m_rssi;
}

void
RssiTag::Set(dBm_u rssi)
{
    m_rssi = rssi;
}

dBm_u
RssiTag::Get() const
{
    return m_rssi;
}

} // namespace ns3
