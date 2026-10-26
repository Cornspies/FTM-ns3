/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef FTM_ASSOC_MANAGER_H
#define FTM_ASSOC_MANAGER_H

#include "wifi-default-assoc-manager.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object-factory.h"
#include "ns3/traced-callback.h"

#include <list>
#include <map>
#include <optional>

namespace ns3
{

class FtmParamsHolder;
class FtmSession;

/**
 * @ingroup wifi
 *
 * Association Manager that uses Fine Timing Measurement (FTM) ranging with the candidate
 * APs to choose the AP to associate with.
 *
 * Scanning is performed as by the WifiDefaultAssocManager. When scanning ends, the
 * candidate APs selected by the configured strategy are ranged one after the other through
 * FTM sessions initiated by the (not yet associated) STA. The candidates are then ordered
 * by the measured distance and the STA attempts to associate with the first one.
 *
 * The available strategies are:
 * - StrongestRssi: no ranging, the AP with the highest SNR is chosen (baseline; equivalent
 *   to the WifiDefaultAssocManager);
 * - ClosestAp: all candidate APs are ranged and the closest one is chosen;
 * - RssiFiltered: only the strongest candidates (at most MaxRangedAps, within RssiMargin of
 *   the strongest one) are ranged and the closest of them is chosen;
 * - Combined: only candidates whose SNR is at least MinSnr are considered; they are ranged
 *   and the closest of them is chosen.
 *
 * Ranging is skipped if fewer than two candidates are left, or if SkipRangingMargin is
 * positive and the strongest candidate exceeds the second strongest by at least that margin.
 * APs that could not be ranged are ordered after the ranged ones, by decreasing SNR.
 *
 * FTM must be enabled on the STA (WifiMac::FTM_Enabled) and on the APs.
 */
class FtmAssocManager : public WifiDefaultAssocManager
{
  public:
    /// Strategies to choose the AP to associate with
    enum Strategy : uint8_t
    {
        STRONGEST_RSSI = 0,
        CLOSEST_AP,
        RSSI_FILTERED,
        COMBINED
    };

    /// Result of ranging with a candidate AP
    struct RangingResult
    {
        Mac48Address bssid;             ///< BSSID of the AP
        double snr{0};                  ///< SNR (linear scale) of the AP at the end of scanning
        std::size_t nRtts{0};           ///< number of FTM dialogs reported by the session
        std::size_t nValidRtts{0};      ///< number of dialogs with all time stamps (RTT != 0)
        std::optional<double> distance; ///< distance estimate in meters, if any valid RTT
        std::optional<double> rssi;     ///< mean RSSI (dBm) of the FTM frames of valid dialogs
        std::optional<double> rssiDistance; ///< distance in meters estimated from the RSSI
        Time start;                     ///< time the FTM session was started
        Time duration;                  ///< duration of the FTM session
        bool timedOut{false};           ///< whether the RangingTimeout expired
    };

    /// Outcome of the AP selection at the end of a scanning procedure
    struct Decision
    {
        Strategy strategy{STRONGEST_RSSI};  ///< strategy used
        std::size_t nCandidates{0};         ///< number of candidate APs
        std::size_t nRanged{0};             ///< number of APs ranging was attempted with
        std::optional<Mac48Address> bssid;  ///< BSSID of the first AP in the sorted list, if any
        double snr{0};                      ///< SNR (linear scale) of that AP
        std::optional<double> distance;     ///< distance estimate of that AP, if ranged
        std::optional<Mac48Address> strongestBssid; ///< BSSID of the AP with the highest SNR
        Time scanningEnd;                   ///< time scanning ended
        Time rangingDuration;               ///< time spent ranging with the candidate APs
    };

    /**
     * TracedCallback signature for the end of ranging with an AP.
     *
     * @param result the ranging result
     */
    typedef void (*RangingResultCallback)(const RangingResult& result);

    /**
     * TracedCallback signature for the AP selection.
     *
     * @param decision the AP selection
     */
    typedef void (*DecisionCallback)(const Decision& decision);

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    FtmAssocManager();
    ~FtmAssocManager() override;

    bool Compare(const StaWifiMac::ApInfo& lhs, const StaWifiMac::ApInfo& rhs) const override;

    /**
     * Estimate the distance to an AP from an RSSI with the log-distance path loss model given
     * by the attributes RssiTxPower, RssiReferenceLoss and RssiExponent (reference distance
     * 1 m): d = 10^((RssiTxPower - RssiReferenceLoss - rssi) / (10 * RssiExponent)) m.
     * The estimate is only used for comparison with FTM; it does not affect the AP selection.
     *
     * @param rssi the RSSI in dBm
     * @return the distance estimate in meters
     */
    double EstimateDistanceFromRssi(double rssi) const;

  protected:
    void DoDispose() override;
    void EndScanning() override;

  private:
    /**
     * @param apInfo the information about an AP
     * @return whether the AP can be chosen by the Combined strategy (SNR at least MinSnr);
     *         always true for the other strategies
     */
    bool IsEligible(const StaWifiMac::ApInfo& apInfo) const;

    /**
     * Remove the distance estimates of the previous scanning procedure.
     */
    void ClearDistances();

    /**
     * @return the candidate APs to range with according to the strategy, strongest first
     */
    std::list<RangingResult> SelectApsToRange() const;

    /**
     * Start an FTM session with the next AP to range with or, if there is none, end ranging.
     */
    void RangeNext();

    /**
     * Callback invoked when an FTM session initiated by this manager is over.
     *
     * @param session the FTM session
     */
    void SessionOver(FtmSession session);

    /**
     * Invoked if the FTM session with the current AP does not end within RangingTimeout.
     */
    void RangingTimeout();

    /**
     * Store and trace the result of ranging with the current AP and continue with the next one.
     */
    void CompleteRanging();

    /**
     * Trace the AP selection and hand over to WifiDefaultAssocManager::EndScanning.
     */
    void EndRanging();

    Strategy m_strategy;               ///< strategy to choose the AP
    uint32_t m_maxRangedAps;           ///< RssiFiltered: maximum number of APs to range with
    double m_rssiMargin;               ///< RssiFiltered: max SNR difference to the strongest (dB)
    double m_minSnr;                   ///< Combined: minimum SNR of a candidate (dB)
    double m_skipRangingMargin;        ///< SNR advantage of the strongest AP to skip ranging (dB)
    Ptr<FtmParamsHolder> m_ftmParams;  ///< FTM parameters of the sessions
    ObjectFactory m_errorModelFactory; ///< factory for the FTM error model of each session
    Time m_rangingTimeout;             ///< maximum duration of the FTM session with one AP
    double m_rssiTxPower;              ///< RSSI distance model: transmit power of the APs (dBm)
    double m_rssiReferenceLoss;        ///< RSSI distance model: path loss at 1 m (dB)
    double m_rssiExponent;             ///< RSSI distance model: path loss exponent

    std::map<Mac48Address, double> m_distances; ///< distance estimates by BSSID
    std::list<RangingResult> m_toRange;          ///< APs still to range with
    std::optional<RangingResult> m_current;      ///< AP currently ranged with
    bool m_ranging{false};                       ///< whether ranging is in progress
    Decision m_decision;                         ///< AP selection being prepared
    EventId m_timeoutEvent;                      ///< ranging timeout event

    TracedCallback<const RangingResult&> m_rangingTrace; ///< end of ranging with an AP
    TracedCallback<const Decision&> m_decisionTrace;     ///< AP selection
};

/**
 * @param os the output stream
 * @param result the ranging result
 * @return the output stream
 */
std::ostream& operator<<(std::ostream& os, const FtmAssocManager::RangingResult& result);

/**
 * @param os the output stream
 * @param decision the AP selection
 * @return the output stream
 */
std::ostream& operator<<(std::ostream& os, const FtmAssocManager::Decision& decision);

} // namespace ns3

#endif /* FTM_ASSOC_MANAGER_H */
