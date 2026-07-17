//
//                  Simu5G
//
// Authors: Andras Varga (OpenSim Ltd)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//
#ifndef _BEARER_MANAGEMENT_H_
#define _BEARER_MANAGEMENT_H_
#include "simu5g/common/LteDefs.h"
#include "simu5g/common/LteControlInfo.h"
#include <inet/common/ModuleRefByPar.h>
using namespace omnetpp;
namespace simu5g {
class LteMacBase;
class RlcMux;
class RlcTxEntityBase;
class RlcRxEntityBase;
class UmTxEntity;
class UpperMux;
class DcMux;
class PdcpTxEntityBase;
class PdcpRxEntityBase;
class Registration;

// nascTime / FRER: which RLC/PDCP registry pair (and mux/mac target) a
// DRB's entities belong to. LTE and NR_PRIMARY are the existing native
// legs; NR_SECONDARY is new, for a genuine second independent NR leg
// (e.g. NrNicUeDC's nrRlcMux2/nrMac2/pdcpMux2), NOT to be confused with
// native DC's pdcpDcMux/bypass-entity/X2 mechanism, which solves a
// different problem (single-bearer split, not two independent DRBs).
enum class RlcLeg { LTE, NR_PRIMARY, NR_SECONDARY };

/**
 * @brief RRC Bearer Management — creates and tears down PDCP, RLC and MAC
 *        entities for data radio bearers.
 */
class BearerManagement : public cSimpleModule
{
  private:
    Registration *registration_ = nullptr;
    // PDCP entity types (resolved from NED params)
    cModuleType *pdcpRxEntityModuleType_ = nullptr;
    cModuleType *pdcpTxEntityModuleType_ = nullptr;
    cModuleType *pdcpBypassRxEntityModuleType_ = nullptr;
    cModuleType *pdcpBypassTxEntityModuleType_ = nullptr;
    cModule *nicModule_ = nullptr;  // containing NIC module (parent of all submodules and entities)
    // RLC entity types (resolved from NED params)
    cModuleType *rlcUmTxEntityModuleType_ = nullptr;
    cModuleType *rlcUmRxEntityModuleType_ = nullptr;
    cModuleType *rlcTmTxEntityModuleType_ = nullptr;
    cModuleType *rlcTmRxEntityModuleType_ = nullptr;
    cModuleType *rlcAmTxEntityModuleType_ = nullptr;
    cModuleType *rlcAmRxEntityModuleType_ = nullptr;
    inet::ModuleRefByPar<RlcMux> rlcMuxModule;
    inet::ModuleRefByPar<RlcMux> nrRlcMuxModule;
    inet::ModuleRefByPar<LteMacBase> macModule;
    inet::ModuleRefByPar<LteMacBase> nrMacModule;

    // nascTime / FRER: second NR leg targets. Optional (not present on
    // plain gNBs or non-DC UEs) -- referenced with required=false, so
    // this is a no-op on any NIC that doesn't have nrRlcMux2/nrMac2.
    inet::ModuleRefByPar<RlcMux> nrRlcMuxModule2;
    inet::ModuleRefByPar<LteMacBase> nrMacModule2;

    // nascTime / FRER: which DRB IDs route to the secondary leg (e.g.
    // {4} for the FRER replica DRB), parsed from a comma-separated NED
    // string param. Empty on any NIC not configured for DC-secondary
    // traffic -- the three-way routing check in createOutgoingConnection()/
    // createIncomingConnection() is then always false, identical to
    // pre-patch behavior.
    std::set<int> dcSecondaryDrbIds_;

    // Entity registries (CP owns the lifecycle of all entities)
    std::map<DrbKey, PdcpTxEntityBase *> pdcpTxEntities_;
    std::map<DrbKey, PdcpRxEntityBase *> pdcpRxEntities_;
    std::map<DrbKey, PdcpTxEntityBase *> pdcpBypassTxEntities_;
    std::map<DrbKey, PdcpRxEntityBase *> pdcpBypassRxEntities_;
    std::map<DrbKey, RlcTxEntityBase *> rlcTxEntities_;
    std::map<DrbKey, RlcRxEntityBase *> rlcRxEntities_;
    std::map<DrbKey, RlcTxEntityBase *> nrRlcTxEntities_;
    std::map<DrbKey, RlcRxEntityBase *> nrRlcRxEntities_;

    // nascTime / FRER: second NR leg's own registries -- separate from
    // the primary NR ones above. Needed even at the PDCP level (unlike
    // RLC, which already had an LTE/NR split natively) because pdcpMux
    // is a single shared instance for LTE+NR-primary; our design adds a
    // genuinely separate pdcpMux2 for the secondary leg.
    std::map<DrbKey, PdcpTxEntityBase *> pdcpTxEntities2_;
    std::map<DrbKey, PdcpRxEntityBase *> pdcpRxEntities2_;
    std::map<DrbKey, RlcTxEntityBase *> nrRlcTxEntities2_;
    std::map<DrbKey, RlcRxEntityBase *> nrRlcRxEntities2_;

    void setRlcEntityParams(cModule *entity, bool isNr);
    void setEntityDisplayPosition(cModule *entity, bool isPdcpEntity, cModule *rlcMux, int bearerIndex);
    RlcTxEntityBase *createAndInstallRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr);
    RlcRxEntityBase *createAndInstallRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr);

    // nascTime / FRER: decide which leg a DRB belongs to.
    RlcLeg legForDrb(int drbId, bool isNrUeSide) const;

  protected:
    void initialize(int stage) override;
    int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    void handleMessage(cMessage *msg) override;
  public:
    virtual void createIncomingConnection(FlowControlInfo *lteInfo, bool withPdcp=true);
    virtual void createOutgoingConnection(FlowControlInfo *lteInfo, bool withPdcp=true);
    virtual RlcTxEntityBase *createRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo);
    virtual RlcRxEntityBase *createRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo);
    RlcTxEntityBase *lookupRlcTxBuffer(DrbKey id);
    PdcpTxEntityBase *lookupPdcpTxEntity(DrbKey id);
    PdcpRxEntityBase *lookupPdcpRxEntity(DrbKey id);
    // nascTime / FRER: leg parameter added (was: deleteLocalPdcpEntities(MacNodeId)).
    // Also fixes a real, pre-existing bug: the UE-side branch previously
    // deleted ALL PDCP entities regardless of nodeId, which would silently
    // corrupt an unrelated still-attached leg's state during any
    // independent handover/detach. Now always filters by nodeId.
    virtual void deleteLocalPdcpEntities(MacNodeId nodeId, RlcLeg leg = RlcLeg::LTE);
    virtual void deleteLocalRlcQueues(MacNodeId nodeId, RlcLeg leg = RlcLeg::LTE);
    void pdcpActiveUeUL(std::set<MacNodeId> *ueSet);
};
} // namespace simu5g
#endif
