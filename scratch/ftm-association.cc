/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * FTM-based AP selection during association.
 *
 * One STA and two APs of the same ESS on one 2.4 GHz channel:
 *
 *   AP0 (near)          STA               AP1 (far)
 *     x <-- nearDistance --> x <---- farDistance ----> x
 *
 * AP0 is closer, but its signal is attenuated by obstructionLoss (e.g., walls inside the own
 * bus), so the STA receives AP1 with a higher SNR. The strongest-RSSI strategy picks AP1,
 * the FTM-based strategies should pick AP0.
 *
 * Example:
 *   ./ns3 run "ftm-association --strategy=StrongestRssi"
 *   ./ns3 run "ftm-association --strategy=ClosestAp --errorModel=wired"
 */

#include "ns3/core-module.h"
#include "ns3/ftm-assoc-manager.h"
#include "ns3/ftm-error-model.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/propagation-module.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/wifi-module.h"

#include <iomanip>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("FtmAssociation");

namespace
{
std::map<Mac48Address, std::string> g_apNames; ///< AP names by BSSID
Time g_scanStart;                              ///< time the STA started scanning

std::string
ApName(const std::optional<Mac48Address>& bssid)
{
    if (!bssid)
    {
        return "none";
    }
    auto it = g_apNames.find(*bssid);
    return it != g_apNames.end() ? it->second : "unknown";
}

std::string
Distance(const std::optional<double>& distance)
{
    std::ostringstream os;
    if (distance)
    {
        os << std::fixed << std::setprecision(2) << *distance << " m";
    }
    else
    {
        os << "-";
    }
    return os.str();
}

void
RangingResult(const FtmAssocManager::RangingResult& result)
{
    std::cout << "ranging   " << ApName(result.bssid) << ": distance " << Distance(result.distance)
              << ", valid RTTs " << result.nValidRtts << "/" << result.nRtts << ", SNR "
              << std::fixed << std::setprecision(1) << RatioToDb(result.snr) << " dB, took "
              << result.duration.As(Time::MS) << (result.timedOut ? " (timed out)" : "")
              << std::endl;
}

void
Decision(const FtmAssocManager::Decision& decision)
{
    std::cout << "decision  " << ApName(decision.bssid) << " (strongest: "
              << ApName(decision.strongestBssid) << "), " << decision.nRanged << " of "
              << decision.nCandidates << " candidates ranged, ranging took "
              << decision.rangingDuration.As(Time::MS) << std::endl;
}

void
Associated(Mac48Address bssid)
{
    std::cout << "associated with " << ApName(bssid) << " after "
              << (Simulator::Now() - g_scanStart).As(Time::MS) << std::endl;
}
} // namespace

int
main(int argc, char* argv[])
{
    // FTM time stamps need picosecond resolution; set it before any Time value is created
    Time::SetResolution(Time::PS);

    std::string strategy = "ClosestAp";
    std::string errorModel = "ideal";
    std::string mapFile = "src/wifi/ftm_map/dim_16.map";
    double nearDistance = 3;
    double farDistance = 6;
    double obstructionLoss = 15;
    double txPower = 16;
    uint32_t ftmsPerBurst = 8;
    bool ftmOnAp0 = true;

    CommandLine cmd(__FILE__);
    cmd.AddValue("strategy", "StrongestRssi, ClosestAp, RssiFiltered or Combined", strategy);
    cmd.AddValue("errorModel", "FTM error model: ideal, wired, wireless or sigstr", errorModel);
    cmd.AddValue("mapFile", "FTM error map for the wireless and sigstr error models", mapFile);
    cmd.AddValue("nearDistance", "Distance between the STA and AP0 (m)", nearDistance);
    cmd.AddValue("farDistance", "Distance between the STA and AP1 (m)", farDistance);
    cmd.AddValue("obstructionLoss", "Extra loss on the link to AP0 (dB)", obstructionLoss);
    cmd.AddValue("txPower", "Transmit power of all devices (dBm)", txPower);
    cmd.AddValue("ftmsPerBurst", "Number of FTM frames per ranging session", ftmsPerBurst);
    cmd.AddValue("ftmOnAp0", "Whether AP0 supports FTM", ftmOnAp0);
    cmd.Parse(argc, argv);

    Config::SetDefault("ns3::WifiMac::FTM_Enabled", BooleanValue(true));

    NodeContainer apNodes(2);
    NodeContainer staNode(1);

    MobilityHelper mobility;
    auto positions = CreateObject<ListPositionAllocator>();
    positions->Add(Vector(-nearDistance, 0, 0)); // AP0
    positions->Add(Vector(farDistance, 0, 0));   // AP1
    positions->Add(Vector(0, 0, 0));             // STA
    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNodes);
    mobility.Install(staNode);

    // log-distance path loss for all links, plus the obstruction on the links with AP0
    auto obstruction = CreateObject<MatrixPropagationLossModel>();
    obstruction->SetDefaultLoss(0);
    obstruction->SetLoss(apNodes.Get(0)->GetObject<MobilityModel>(),
                         staNode.Get(0)->GetObject<MobilityModel>(),
                         obstructionLoss);
    auto pathLoss = CreateObject<LogDistancePropagationLossModel>();
    pathLoss->SetNext(obstruction);

    auto channel = CreateObject<YansWifiChannel>();
    channel->SetPropagationLossModel(pathLoss);
    channel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    YansWifiPhyHelper phy;
    phy.SetChannel(channel);
    phy.Set("ChannelSettings", StringValue("{1, 20, BAND_2_4GHZ, 0}"));
    phy.Set("TxPowerStart", DoubleValue(txPower));
    phy.Set("TxPowerEnd", DoubleValue(txPower));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);

    Ssid ssid("bus");
    WifiMacHelper mac;
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    auto apDevices = wifi.Install(phy, mac, apNodes);
    if (!ftmOnAp0)
    {
        DynamicCast<WifiNetDevice>(apDevices.Get(0))->GetMac()->DisableFtm();
    }
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(true));
    auto staDevices = wifi.Install(phy, mac, staNode);

    for (uint32_t i = 0; i < apDevices.GetN(); ++i)
    {
        g_apNames[Mac48Address::ConvertFrom(apDevices.Get(i)->GetAddress())] =
            "AP" + std::to_string(i) + (i == 0 ? " (near)" : " (far)");
    }

    // FTM-based association manager; installed here to keep a handle for the trace sources
    auto assocManager = CreateObject<FtmAssocManager>();
    assocManager->SetAttribute("Strategy", StringValue(strategy));

    const std::map<std::string, std::string> errorModels{
        {"ideal", "ns3::FtmErrorModel"},
        {"wired", "ns3::WiredFtmErrorModel"},
        {"wireless", "ns3::WirelessFtmErrorModel"},
        {"sigstr", "ns3::WirelessSigStrFtmErrorModel"},
    };
    NS_ABORT_MSG_IF(!errorModels.contains(errorModel), "Unknown error model " << errorModel);
    assocManager->SetAttribute("ErrorModel", StringValue(errorModels.at(errorModel)));
    if (errorModel == "wireless" || errorModel == "sigstr")
    {
        auto map = CreateObject<WirelessFtmErrorModel::FtmMap>();
        map->LoadMap(mapFile);
        Config::SetDefault("ns3::WirelessFtmErrorModel::FtmMap", PointerValue(map));
    }

    PointerValue params;
    assocManager->GetAttribute("FtmParams", params);
    auto ftmParams = params.Get<FtmParamsHolder>()->GetFtmParams();
    ftmParams.SetFtmsPerBurst(ftmsPerBurst);
    params.Get<FtmParamsHolder>()->SetFtmParams(ftmParams);

    auto staMac =
        DynamicCast<StaWifiMac>(DynamicCast<WifiNetDevice>(staDevices.Get(0))->GetMac());
    staMac->SetAssocManager(assocManager);

    assocManager->TraceConnectWithoutContext("RangingResult", MakeCallback(&RangingResult));
    assocManager->TraceConnectWithoutContext("Decision", MakeCallback(&Decision));
    staMac->TraceConnectWithoutContext("Assoc", MakeCallback(&Associated));

    std::cout << "strategy " << strategy << ", error model " << errorModel << "; AP0 (near) at "
              << nearDistance << " m with " << obstructionLoss << " dB obstruction, AP1 (far) at "
              << farDistance << " m" << std::endl;

    // the STA starts scanning when it is initialized, at the beginning of the simulation
    g_scanStart = Seconds(0);
    Simulator::Stop(Seconds(2));
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
