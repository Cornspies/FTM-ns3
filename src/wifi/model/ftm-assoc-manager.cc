/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ftm-assoc-manager.h"

#include "ftm-error-model.h"
#include "ftm-header.h"
#include "ftm-session.h"
#include "wifi-utils.h"

#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-net-device.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FtmAssocManager");

NS_OBJECT_ENSURE_REGISTERED(FtmAssocManager);

namespace
{
/// speed of light in m/s
constexpr double SPEED_OF_LIGHT = 299792458.0;
} // namespace

TypeId
FtmAssocManager::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::FtmAssocManager")
            .SetParent<WifiDefaultAssocManager>()
            .AddConstructor<FtmAssocManager>()
            .SetGroupName("Wifi")
            .AddAttribute("Strategy",
                          "The strategy to choose the AP to associate with.",
                          EnumValue(FtmAssocManager::CLOSEST_AP),
                          MakeEnumAccessor<Strategy>(&FtmAssocManager::m_strategy),
                          MakeEnumChecker(FtmAssocManager::STRONGEST_RSSI,
                                          "StrongestRssi",
                                          FtmAssocManager::CLOSEST_AP,
                                          "ClosestAp",
                                          FtmAssocManager::RSSI_FILTERED,
                                          "RssiFiltered",
                                          FtmAssocManager::COMBINED,
                                          "Combined"))
            .AddAttribute("MaxRangedAps",
                          "RssiFiltered strategy: range with at most this number of the "
                          "strongest candidate APs (0 means no limit).",
                          UintegerValue(3),
                          MakeUintegerAccessor(&FtmAssocManager::m_maxRangedAps),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RssiMargin",
                          "RssiFiltered strategy: range only with candidate APs whose SNR is at "
                          "most this amount (dB) below the SNR of the strongest candidate.",
                          DoubleValue(10),
                          MakeDoubleAccessor(&FtmAssocManager::m_rssiMargin),
                          MakeDoubleChecker<double>(0))
            .AddAttribute("MinSnr",
                          "Combined strategy: minimum SNR (dB) of a candidate AP. APs below "
                          "this SNR are only chosen if no AP reaches it.",
                          DoubleValue(10),
                          MakeDoubleAccessor(&FtmAssocManager::m_minSnr),
                          MakeDoubleChecker<double>())
            .AddAttribute("SkipRangingMargin",
                          "Skip ranging if the strongest candidate AP exceeds the SNR of the "
                          "second strongest by at least this amount (dB). 0 disables this rule.",
                          DoubleValue(0),
                          MakeDoubleAccessor(&FtmAssocManager::m_skipRangingMargin),
                          MakeDoubleChecker<double>(0))
            .AddAttribute("FtmParams",
                          "The FTM parameters of the sessions with the candidate APs. By "
                          "default, one ASAP burst of 8 FTM frames, 100 us apart, within 16 ms.",
                          PointerValue(),
                          MakePointerAccessor(&FtmAssocManager::m_ftmParams),
                          MakePointerChecker<FtmParamsHolder>())
            .AddAttribute("ErrorModel",
                          "The FTM error model used by the sessions with the candidate APs. A "
                          "new instance is created for each session; error models derived from "
                          "WirelessFtmErrorModel are attached to the node of the STA. The "
                          "default (ns3::FtmErrorModel) adds no error.",
                          // by TypeId, as FtmErrorModel may not be registered yet by name
                          ObjectFactoryValue([] {
                              ObjectFactory factory;
                              factory.SetTypeId(FtmErrorModel::GetTypeId());
                              return factory;
                          }()),
                          MakeObjectFactoryAccessor(&FtmAssocManager::m_errorModelFactory),
                          MakeObjectFactoryChecker())
            .AddAttribute("RangingTimeout",
                          "Give up ranging with a candidate AP if the FTM session does not end "
                          "within this time.",
                          TimeValue(MilliSeconds(100)),
                          MakeTimeAccessor(&FtmAssocManager::m_rangingTimeout),
                          MakeTimeChecker(Seconds(0), Seconds(10)))
            .AddAttribute("RssiTxPower",
                          "Transmit power of the APs (dBm) assumed to estimate the distance from "
                          "the RSSI of the FTM frames (for comparison only). The default is the "
                          "default TxPowerStart of WifiPhy.",
                          DoubleValue(16.0206),
                          MakeDoubleAccessor(&FtmAssocManager::m_rssiTxPower),
                          MakeDoubleChecker<double>())
            .AddAttribute("RssiReferenceLoss",
                          "Path loss at 1 m (dB) assumed to estimate the distance from the RSSI. "
                          "The default is that of LogDistancePropagationLossModel.",
                          DoubleValue(46.6777),
                          MakeDoubleAccessor(&FtmAssocManager::m_rssiReferenceLoss),
                          MakeDoubleChecker<double>())
            .AddAttribute("RssiExponent",
                          "Path loss exponent assumed to estimate the distance from the RSSI. The "
                          "default is that of LogDistancePropagationLossModel.",
                          DoubleValue(3.0),
                          MakeDoubleAccessor(&FtmAssocManager::m_rssiExponent),
                          MakeDoubleChecker<double>(0))
            .AddTraceSource("RangingResult",
                            "Ranging with a candidate AP has ended.",
                            MakeTraceSourceAccessor(&FtmAssocManager::m_rangingTrace),
                            "ns3::FtmAssocManager::RangingResultCallback")
            .AddTraceSource("Decision",
                            "The candidate APs have been sorted at the end of a scanning "
                            "procedure; the STA attempts to associate with the first one.",
                            MakeTraceSourceAccessor(&FtmAssocManager::m_decisionTrace),
                            "ns3::FtmAssocManager::DecisionCallback");
    return tid;
}

FtmAssocManager::FtmAssocManager()
{
    NS_LOG_FUNCTION(this);

    FtmParams params;
    params.SetStatusIndication(FtmParams::RESERVED);
    params.SetStatusIndicationValue(0);
    params.SetNumberOfBurstsExponent(0); // 1 burst
    params.SetBurstDuration(8);          // 16 ms
    params.SetMinDeltaFtm(1);            // 100 us between FTM frames
    params.SetPartialTsfNoPref(true);
    params.SetAsap(true);
    params.SetFtmsPerBurst(8);
    params.SetBurstPeriod(0); // must be 0 with one burst
    m_ftmParams = CreateObject<FtmParamsHolder>();
    m_ftmParams->SetFtmParams(params);
}

FtmAssocManager::~FtmAssocManager()
{
    NS_LOG_FUNCTION_NOARGS();
}

void
FtmAssocManager::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_timeoutEvent.Cancel();
    m_ftmParams = nullptr;
    m_toRange.clear();
    m_current.reset();
    m_distances.clear();
    WifiDefaultAssocManager::DoDispose();
}

double
FtmAssocManager::EstimateDistanceFromRssi(double rssi) const
{
    return std::pow(10.0, (m_rssiTxPower - m_rssiReferenceLoss - rssi) / (10 * m_rssiExponent));
}

bool
FtmAssocManager::IsEligible(const StaWifiMac::ApInfo& apInfo) const
{
    return m_strategy != COMBINED || RatioToDb(apInfo.m_snr) >= m_minSnr;
}

bool
FtmAssocManager::Compare(const StaWifiMac::ApInfo& lhs, const StaWifiMac::ApInfo& rhs) const
{
    // group 0: eligible and ranged, group 1: eligible but not ranged, group 2: not eligible
    auto group = [this](const StaWifiMac::ApInfo& ap, const std::optional<double>& distance) {
        return !IsEligible(ap) ? 2 : (distance ? 0 : 1);
    };
    auto distance = [this](const StaWifiMac::ApInfo& ap) -> std::optional<double> {
        if (auto it = m_distances.find(ap.m_bssid); it != m_distances.end())
        {
            return it->second;
        }
        return std::nullopt;
    };

    const auto lhsDistance = distance(lhs);
    const auto rhsDistance = distance(rhs);
    const auto lhsGroup = group(lhs, lhsDistance);
    const auto rhsGroup = group(rhs, rhsDistance);

    if (lhsGroup != rhsGroup)
    {
        return lhsGroup < rhsGroup;
    }
    if (lhsGroup == 0 && *lhsDistance != *rhsDistance)
    {
        return *lhsDistance < *rhsDistance;
    }
    if (lhs.m_snr != rhs.m_snr)
    {
        return lhs.m_snr > rhs.m_snr;
    }
    // APs with the same distance and SNR must not compare equal, otherwise the sorted list
    // considers them duplicates
    return lhs.m_bssid < rhs.m_bssid;
}

void
FtmAssocManager::ClearDistances()
{
    NS_LOG_FUNCTION(this);
    while (!m_distances.empty())
    {
        auto bssid = m_distances.begin()->first;
        m_distances.erase(m_distances.begin());
        Resort(bssid);
    }
}

std::list<FtmAssocManager::RangingResult>
FtmAssocManager::SelectApsToRange() const
{
    NS_LOG_FUNCTION(this);

    if (m_strategy == STRONGEST_RSSI)
    {
        return {};
    }

    std::vector<const StaWifiMac::ApInfo*> candidates;
    for (const auto& apInfo : GetSortedList())
    {
        if (IsEligible(apInfo))
        {
            candidates.push_back(&apInfo);
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](auto lhs, auto rhs) {
        return lhs->m_snr > rhs->m_snr;
    });

    if (m_strategy == RSSI_FILTERED && !candidates.empty())
    {
        const auto minSnr = RatioToDb(candidates.front()->m_snr) - m_rssiMargin;
        std::erase_if(candidates, [minSnr](auto ap) { return RatioToDb(ap->m_snr) < minSnr; });
        if (m_maxRangedAps > 0 && candidates.size() > m_maxRangedAps)
        {
            candidates.resize(m_maxRangedAps);
        }
    }

    if (candidates.size() < 2)
    {
        NS_LOG_DEBUG("Fewer than two candidate APs, no ranging");
        return {};
    }
    if (m_skipRangingMargin > 0 &&
        RatioToDb(candidates[0]->m_snr) - RatioToDb(candidates[1]->m_snr) >= m_skipRangingMargin)
    {
        NS_LOG_DEBUG("Strongest AP " << candidates[0]->m_bssid << " exceeds the second strongest "
                                     << "by at least " << m_skipRangingMargin
                                     << " dB, no ranging");
        return {};
    }

    std::list<RangingResult> toRange;
    for (const auto ap : candidates)
    {
        toRange.push_back({.bssid = ap->m_bssid, .snr = ap->m_snr});
    }
    return toRange;
}

void
FtmAssocManager::EndScanning()
{
    NS_LOG_FUNCTION(this);

    if (m_ranging)
    {
        NS_LOG_DEBUG("Ranging already in progress");
        return;
    }

    ClearDistances();

    m_decision = Decision{};
    m_decision.strategy = m_strategy;
    m_decision.nCandidates = GetSortedList().size();
    m_decision.scanningEnd = Simulator::Now();
    if (auto strongest = std::max_element(GetSortedList().cbegin(),
                                          GetSortedList().cend(),
                                          [](auto&& lhs, auto&& rhs) {
                                              return lhs.m_snr < rhs.m_snr;
                                          });
        strongest != GetSortedList().cend())
    {
        m_decision.strongestBssid = strongest->m_bssid;
    }

    m_toRange = SelectApsToRange();
    m_decision.nRanged = m_toRange.size();
    if (m_toRange.empty())
    {
        EndRanging();
        return;
    }

    NS_ABORT_MSG_IF(!m_mac->GetFtmEnabled(),
                    "FtmAssocManager needs FTM to be enabled on the STA (WifiMac::FTM_Enabled)");
    m_ranging = true;
    RangeNext();
}

void
FtmAssocManager::RangeNext()
{
    NS_LOG_FUNCTION(this);

    if (m_toRange.empty())
    {
        m_ranging = false;
        EndRanging();
        return;
    }

    m_current = std::move(m_toRange.front());
    m_toRange.pop_front();
    m_current->start = Simulator::Now();

    auto session = m_mac->NewFtmSession(m_current->bssid);
    if (!session)
    {
        // e.g., a session with this AP that timed out earlier is still in progress
        NS_LOG_DEBUG("Could not create an FTM session with " << m_current->bssid);
        CompleteRanging();
        return;
    }

    NS_LOG_DEBUG("Ranging with " << m_current->bssid);
    session->SetFtmParams(m_ftmParams->GetFtmParams());
    auto errorModel = m_errorModelFactory.Create<FtmErrorModel>();
    NS_ABORT_MSG_IF(!errorModel, "ErrorModel must be an FtmErrorModel");
    if (auto wirelessErrorModel = DynamicCast<WirelessFtmErrorModel>(errorModel))
    {
        wirelessErrorModel->SetNode(m_mac->GetDevice()->GetNode());
    }
    session->SetFtmErrorModel(errorModel);
    session->SetSessionOverCallback(MakeCallback(&FtmAssocManager::SessionOver, this));
    m_timeoutEvent = Simulator::Schedule(m_rangingTimeout, &FtmAssocManager::RangingTimeout, this);
    session->SessionBegin();
}

void
FtmAssocManager::SessionOver(FtmSession session)
{
    NS_LOG_FUNCTION(this << session.GetPartnerAddress());

    if (!m_current || session.GetPartnerAddress() != m_current->bssid)
    {
        NS_LOG_DEBUG("Ignoring the end of a session with " << session.GetPartnerAddress()
                                                           << " that has timed out");
        return;
    }
    m_timeoutEvent.Cancel();

    // an RTT of 0 marks a dialog whose time stamps were not all available; the signal strength
    // (dBm) of the FTM frame of each dialog is stored at the same position in its list
    const auto rtts = session.GetIndividualRTT();
    const auto sigStrs = session.GetIndividualSignalStrength();
    NS_ASSERT(rtts.size() == sigStrs.size());
    double sum = 0;
    double rxPowerSum = 0; // mW
    auto sigStr = sigStrs.cbegin();
    for (auto rtt = rtts.cbegin(); rtt != rtts.cend(); ++rtt, ++sigStr)
    {
        if (*rtt != 0)
        {
            sum += *rtt;
            rxPowerSum += std::pow(10.0, *sigStr / 10);
            m_current->nValidRtts++;
        }
    }
    m_current->nRtts = rtts.size();
    if (m_current->nValidRtts > 0)
    {
        // RTTs are in picoseconds
        m_current->distance = sum / m_current->nValidRtts * 1e-12 * SPEED_OF_LIGHT / 2;
        // average the received power in the linear domain
        m_current->rssi = 10 * std::log10(rxPowerSum / m_current->nValidRtts);
        m_current->rssiDistance = EstimateDistanceFromRssi(*m_current->rssi);
    }

    // the session is still being torn down by the FTM manager; continue afterwards
    Simulator::ScheduleNow(&FtmAssocManager::CompleteRanging, this);
}

void
FtmAssocManager::RangingTimeout()
{
    NS_LOG_FUNCTION(this);
    NS_ASSERT(m_current);
    NS_LOG_DEBUG("Ranging with " << m_current->bssid << " timed out");
    m_current->timedOut = true;
    CompleteRanging();
}

void
FtmAssocManager::CompleteRanging()
{
    NS_ASSERT(m_current);
    NS_LOG_FUNCTION(this << *m_current);

    m_current->duration = Simulator::Now() - m_current->start;
    if (m_current->distance)
    {
        m_distances[m_current->bssid] = *m_current->distance;
        Resort(m_current->bssid);
    }
    m_rangingTrace(*m_current);
    m_current.reset();
    RangeNext();
}

void
FtmAssocManager::EndRanging()
{
    NS_LOG_FUNCTION(this);

    m_decision.rangingDuration = Simulator::Now() - m_decision.scanningEnd;
    if (const auto& list = GetSortedList(); !list.empty())
    {
        const auto& first = *list.cbegin();
        m_decision.bssid = first.m_bssid;
        m_decision.snr = first.m_snr;
        if (auto it = m_distances.find(first.m_bssid); it != m_distances.end())
        {
            m_decision.distance = it->second;
        }
    }
    NS_LOG_DEBUG(m_decision);
    m_decisionTrace(m_decision);

    WifiDefaultAssocManager::EndScanning();
}

std::ostream&
operator<<(std::ostream& os, const FtmAssocManager::RangingResult& result)
{
    os << "bssid=" << result.bssid << " snr=" << RatioToDb(result.snr) << "dB"
       << " rtts=" << result.nValidRtts << "/" << result.nRtts << " distance=";
    if (result.distance)
    {
        os << *result.distance << "m";
    }
    else
    {
        os << "none";
    }
    if (result.rssi)
    {
        os << " rssi=" << *result.rssi << "dBm rssiDistance=" << *result.rssiDistance << "m";
    }
    os << " start=" << result.start.As(Time::US) << " duration=" << result.duration.As(Time::US)
       << (result.timedOut ? " (timed out)" : "");
    return os;
}

std::ostream&
operator<<(std::ostream& os, const FtmAssocManager::Decision& decision)
{
    os << "strategy=" << +decision.strategy << " candidates=" << decision.nCandidates
       << " ranged=" << decision.nRanged << " chosen=";
    if (decision.bssid)
    {
        os << *decision.bssid << " snr=" << RatioToDb(decision.snr) << "dB distance=";
        if (decision.distance)
        {
            os << *decision.distance << "m";
        }
        else
        {
            os << "none";
        }
    }
    else
    {
        os << "none";
    }
    os << " strongest=";
    if (decision.strongestBssid)
    {
        os << *decision.strongestBssid;
    }
    else
    {
        os << "none";
    }
    os << " rangingDuration=" << decision.rangingDuration.As(Time::US);
    return os;
}

} // namespace ns3
