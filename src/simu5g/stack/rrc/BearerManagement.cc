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

#include <sstream>

#include "simu5g/stack/rrc/BearerManagement.h"
#include "simu5g/stack/rrc/D2DModeController.h"
#include "simu5g/stack/rrc/Registration.h"
#include "simu5g/stack/mac/LteMacBase.h"
#include "simu5g/stack/rlc/RlcMux.h"
#include "simu5g/stack/rlc/um/UmTxEntity.h"
#include "simu5g/stack/pdcp/UpperMux.h"
#include "simu5g/stack/pdcp/DcMux.h"
#include "simu5g/stack/pdcp/PdcpTxEntityBase.h"
#include "simu5g/stack/pdcp/PdcpRxEntityBase.h"
#include "simu5g/common/InitStages.h"

namespace simu5g {

using namespace inet;

Define_Module(BearerManagement);

void BearerManagement::initialize(int stage)
{
    if (stage == inet::INITSTAGE_LOCAL) {
        registration_ = check_and_cast<Registration *>(getParentModule()->getSubmodule("registration"));

        // Resolve PDCP entity types
        pdcpTxEntityModuleType_ = cModuleType::get(par("pdcpTxEntityModuleType").stringValue());
        pdcpRxEntityModuleType_ = cModuleType::get(par("pdcpRxEntityModuleType").stringValue());
        pdcpBypassTxEntityModuleType_ = cModuleType::get(par("pdcpBypassTxEntityModuleType").stringValue());
        pdcpBypassRxEntityModuleType_ = cModuleType::get(par("pdcpBypassRxEntityModuleType").stringValue());

        // Resolve RLC entity types
        rlcUmTxEntityModuleType_ = cModuleType::get(par("rlcUmTxEntityModuleType").stringValue());
        rlcUmRxEntityModuleType_ = cModuleType::get(par("rlcUmRxEntityModuleType").stringValue());
        rlcTmTxEntityModuleType_ = cModuleType::get(par("rlcTmTxEntityModuleType").stringValue());
        rlcTmRxEntityModuleType_ = cModuleType::get(par("rlcTmRxEntityModuleType").stringValue());
        rlcAmTxEntityModuleType_ = cModuleType::get(par("rlcAmTxEntityModuleType").stringValue());
        rlcAmRxEntityModuleType_ = cModuleType::get(par("rlcAmRxEntityModuleType").stringValue());

        nicModule_ = inet::getContainingNicModule(this);

        rlcMuxModule.reference(this, "rlcMuxModule", true);
        nrRlcMuxModule.reference(this, "nrRlcMuxModule", false);
        macModule.reference(this, "macModule", true);
        nrMacModule.reference(this, "nrMacModule", false);

        // nascTime / FRER: optional second NR leg -- unresolved (empty)
        // on any NIC that doesn't have nrRlcMux2/nrMac2.
        nrRlcMuxModule2.reference(this, "nrRlcMuxModule2", false);
        nrMacModule2.reference(this, "nrMacModule2", false);

        std::string drbIdsStr = par("dcSecondaryDrbIds").stdstringValue();
        std::stringstream ss(drbIdsStr);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty())
                dcSecondaryDrbIds_.insert(std::atoi(token.c_str()));
        }
    }
}

void BearerManagement::handleMessage(cMessage *msg)
{
    throw cRuntimeError("This module does not process messages");
}

RlcLeg BearerManagement::legForDrb(DrbId drbId, bool isNr, bool isUeSide) const
{
    if (!isNr)
        return RlcLeg::LTE;
    if (isUeSide && dcSecondaryDrbIds_.count(num(drbId)) && nrRlcMuxModule2 && nrMacModule2)
        return RlcLeg::NR_SECONDARY;
    return RlcLeg::NR_PRIMARY;
}

// ============================================================================
// createOutgoingConnection() — corrected.
// Every line is byte-for-byte pristine EXCEPT where marked "nascTime / FRER".
// The gNB-side (NODEB) path is now completely untouched, matching pristine
// exactly -- this is the fix for the null LteMacBase crash: the original
// rewrite routed gNB-side traffic through a unified leg selector that could
// resolve to an unconfigured nrMacModule on a plain gNodeB. Now it never can.
// ============================================================================
void BearerManagement::createOutgoingConnection(FlowControlInfo *lteInfo, bool withPdcp)
{
    Enter_Method_Silent("createOutgoingConnection()");
    EV << "BearerManagement::createOutgoingConnection - " << " srcId=" << lteInfo->getSourceId() << " destId=" << lteInfo->getDestId()
        << " groupId=" << lteInfo->getMulticastGroupId() << " drbId=" << lteInfo->getDrbId()
        << " direction=" << dirToA(lteInfo->getDirection())
        << " withPdcp=" << (withPdcp ? "yes" : "no") << endl;
    ASSERT(lteInfo->getSourceId() == registration_->getLteNodeId() || lteInfo->getSourceId() == registration_->getNrNodeId());

    FlowDescriptor desc = FlowDescriptor::fromFlowControlInfo(*lteInfo);
    MacNodeId destId = desc.getDestId();

    // nascTime / FRER: is this a UE-side, NR, DC-secondary-routed DRB?
    // Computed ONCE and used only to select the "2" targets on the UE-side
    // branch below -- the NODEB (gNB) branch is completely unaffected in
    // every line below, matching pristine exactly, since a plain gNodeB
    // never has a second leg of its own (confirmed weeks ago).
    bool isUeSide = (registration_->getNodeType()==UE);
    bool isNrDrb = isUeSide && isNrUe(lteInfo->getSourceId());
    bool isDcSecondaryDrb = isNrDrb && dcSecondaryDrbIds_.count(num(desc.getDrbId())) && nrMacModule2 && nrRlcMuxModule2;

    // Create MAC outgoing connection
    auto mac = (registration_->getNodeType()==UE && isNrUe(lteInfo->getSourceId()))
        ? (isDcSecondaryDrb ? nrMacModule2.get() : nrMacModule.get())
        : macModule.get();
    LogicalCid lcid = mac->drbIdToLcid(desc.getDrbId());
    MacCid cid = MacCid(destId, lcid);
    mac->createOutgoingConnection(cid, desc);

    // RLC entity creation
    DrbKey rlcId = ctrlInfoToTxDrbKey(lteInfo);
    bool isNr = (registration_->getNodeType()==UE && isNrUe(lteInfo->getSourceId()));
    auto *rlcMux = isNr
        ? (isDcSecondaryDrb ? nrRlcMuxModule2.get() : nrRlcMuxModule.get())
        : rlcMuxModule.get();
    createAndInstallRlcTxBuffer(rlcId, lteInfo, rlcMux, isNr);

    // PDCP entity creation
    // nascTime / FRER: pdcpMux selection is the ONE place a genuinely new
    // pdcpMux2 instance is needed (pristine has no leg distinction here at
    // all -- confirmed, always plain "pdcpMux" -- because RLC already had
    // an LTE/NR split natively but PDCP never did until this design added
    // a second, independent NR leg).
    auto *pdcpMux = isDcSecondaryDrb
        ? check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux2"))
        : check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux")); // nullptr on UEs (no X2)

    if (withPdcp) {
        DrbKey id = DrbKey(lteInfo->getDestId(), lteInfo->getDrbId());
        // DC UE NR leg: check if a master (LTE-leg) PDCP TX entity exists for the same DRB.
        // If so, wire its nrOut gate to the NR RLC TX entity instead of creating a new PDCP entity.
        // nascTime / FRER: this native EN-DC shortcut must never apply to a
        // DC-SECONDARY DRB -- that's a fully independent DRB (its own PDCP
        // entity in pdcpTxEntities2_), not a split-bearer forwarded one.
        bool wiredToMaster = false;
        if (!isDcSecondaryDrb && registration_->getNodeType()==UE && isNrUe(lteInfo->getSourceId())) {
            for (auto& [key, masterEntity] : pdcpTxEntities_) {
                if (key.getDrbId() == id.getDrbId()) {
                    auto *masterModule = check_and_cast<cModule *>(masterEntity);
                    if (masterModule->hasGate("nrOut")) {
                        auto nrRlcIt = nrRlcTxEntities_.find(rlcId);
                        ASSERT(nrRlcIt != nrRlcTxEntities_.end());
                        masterModule->gate("nrOut")->connectTo(nrRlcIt->second->gate("in"));
                        wiredToMaster = true;
                    }
                    break;
                }
            }
        }
        if (!wiredToMaster) {
            // Normal case: create PDCP TX entity
            std::string name = "pdcp-tx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
            auto *module = pdcpTxEntityModuleType_->create(name.c_str(), nicModule_);
            module->par("headerCompressedSize") = par("headerCompressedSize");
            module->finalizeParameters();
            module->buildInside();
            setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));
            // Wire UpperMux → entity in gate
            int idx = pdcpMux->gateSize("toTxEntity");
            pdcpMux->setGateSize("toTxEntity", idx + 1);
            pdcpMux->gate("toTxEntity", idx)->connectTo(module->gate("in"));
            // Wire PDCP TX out → RLC TX in (direct per-DRB connection)
            // nascTime / FRER: registry selection now three-way, was two-way.
            auto& txRegistry = isDcSecondaryDrb ? nrRlcTxEntities2_
                              : (isNrUe(lteInfo->getSourceId()) ? nrRlcTxEntities_ : rlcTxEntities_);
            auto rlcIt = txRegistry.find(rlcId);
            ASSERT(rlcIt != txRegistry.end());
            module->gate("out")->connectTo(rlcIt->second->gate("in"));
            // Wire dcOut gate → DcMux (if entity has one, e.g. NrTxPdcpEntity; eNB only)
            if (!isDcSecondaryDrb && pdcpDcMux && module->hasGate("dcOut")) {
                int dcIdx = pdcpDcMux->gateSize("fromEntity");
                pdcpDcMux->setGateSize("fromEntity", dcIdx + 1);
                module->gate("dcOut")->connectTo(pdcpDcMux->gate("fromEntity", dcIdx));
            }
            module->scheduleStart(simTime());
            module->callInitialize();
            auto *txEnt = check_and_cast<PdcpTxEntityBase *>(module);
            pdcpMux->registerTxEntity(id, txEnt);
            (isDcSecondaryDrb ? pdcpTxEntities2_ : pdcpTxEntities_)[id] = txEnt;
        }
    }
    else {
        // DC secondary node: create bypass TX entity (forwards DL from master to RLC)
        // Native EN-DC/X2 bypass path -- unreachable for nascTime's
        // DC-secondary DRBs, since patch 7 (Binder::establishUnidirectionalDataConnection)
        // routes them through withPdcp=true always. Unchanged from pristine.
        ASSERT(pdcpDcMux != nullptr); // bypass entities are eNB-only
        DrbKey id = DrbKey(lteInfo->getDestId(), lteInfo->getDrbId());
        std::string name = "pdcp-bypass-tx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpBypassTxEntityModuleType_->create(name.c_str(), nicModule_);
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));
        int idx = pdcpDcMux->gateSize("toBypassTxEntity");
        pdcpDcMux->setGateSize("toBypassTxEntity", idx + 1);
        pdcpDcMux->gate("toBypassTxEntity", idx)->connectTo(module->gate("in"));
        auto rlcIt2 = (isNrUe(lteInfo->getSourceId()) ? nrRlcTxEntities_ : rlcTxEntities_).find(rlcId);
        ASSERT(rlcIt2 != (isNrUe(lteInfo->getSourceId()) ? nrRlcTxEntities_ : rlcTxEntities_).end());
        module->gate("out")->connectTo(rlcIt2->second->gate("in"));
        module->scheduleStart(simTime());
        module->callInitialize();
        auto *txEnt = check_and_cast<PdcpTxEntityBase *>(module);
        pdcpDcMux->registerBypassTxEntity(id, txEnt);
        pdcpBypassTxEntities_[id] = txEnt;
    }
}

// ============================================================================
// createIncomingConnection() — corrected, same pattern, same discipline.
// Original isNr line's "//TODO FIXME! DOES NOT WORK FOR MULTICAST!!!!!"
// comment preserved verbatim -- not our bug to fix, not touching it.
// ============================================================================
void BearerManagement::createIncomingConnection(FlowControlInfo *lteInfo, bool withPdcp)
{
    Enter_Method_Silent("createIncomingConnection()");
    EV << "BearerManagement::createIncomingConnection - " << " srcId=" << lteInfo->getSourceId() << " destId=" << lteInfo->getDestId()
        << " groupId=" << lteInfo->getMulticastGroupId() << " drbId=" << lteInfo->getDrbId()
        << " direction=" << dirToA(lteInfo->getDirection())
        << " withPdcp=" << (withPdcp ? "yes" : "no") << endl;
    ASSERT(lteInfo->getDestId() == registration_->getLteNodeId() || lteInfo->getDestId() == registration_->getNrNodeId() || lteInfo->getMulticastGroupId() != NODEID_NONE);

    FlowDescriptor desc = FlowDescriptor::fromFlowControlInfo(*lteInfo);
    MacNodeId senderId = desc.getSourceId();

    // nascTime / FRER: same UE-side-only DC-secondary check as outgoing.
    bool isUeSide = (registration_->getNodeType()==UE);
    bool isNrDrb = isUeSide && isNrUe(lteInfo->getDestId());
    bool isDcSecondaryDrb = isNrDrb && dcSecondaryDrbIds_.count(num(desc.getDrbId())) && nrMacModule2 && nrRlcMuxModule2;

    auto mac = (registration_->getNodeType()==UE && isNrUe(lteInfo->getDestId()))
        ? (isDcSecondaryDrb ? nrMacModule2.get() : nrMacModule.get())
        : macModule.get(); //TODO FIXME! DOES NOT WORK FOR MULTICAST!!!!!
    LogicalCid lcid = mac->drbIdToLcid(desc.getDrbId());
    MacCid cid = MacCid(senderId, lcid);
    mac->createIncomingConnection(cid, desc);

    // RLC entity creation
    DrbKey rlcId = ctrlInfoToRxDrbKey(lteInfo);
    bool isNr = (registration_->getNodeType()==UE && isNrUe(lteInfo->getDestId())); //TODO FIXME! DOES NOT WORK FOR MULTICAST!!!!!
    auto *rlcMux = isNr
        ? (isDcSecondaryDrb ? nrRlcMuxModule2.get() : nrRlcMuxModule.get())
        : rlcMuxModule.get();
    createAndInstallRlcRxBuffer(rlcId, lteInfo, rlcMux, isNr);

    // PDCP entity creation
    auto *pdcpMux = isDcSecondaryDrb
        ? check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux2"))
        : check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux")); // nullptr on UEs (no X2)

    if (withPdcp) {
        DrbKey id = DrbKey(lteInfo->getSourceId(), lteInfo->getDrbId());
        std::string name = "pdcp-rx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpRxEntityModuleType_->create(name.c_str(), nicModule_);
        module->par("headerCompressedSize") = par("headerCompressedSize");
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));

        // nascTime / FRER: registry selection now three-way.
        auto& rxRegistry = isDcSecondaryDrb ? nrRlcRxEntities2_
                          : (isNrUe(lteInfo->getDestId()) ? nrRlcRxEntities_ : rlcRxEntities_);
        auto rlcIt = rxRegistry.find(rlcId);
        ASSERT(rlcIt != rxRegistry.end());
        rlcIt->second->gate("out")->connectTo(module->gate("in"));

        int fromIdx = pdcpMux->gateSize("fromRxEntity");
        pdcpMux->setGateSize("fromRxEntity", fromIdx + 1);
        module->gate("out")->connectTo(pdcpMux->gate("fromRxEntity", fromIdx));

        if (!isDcSecondaryDrb && pdcpDcMux && module->hasGate("dcIn")) {
            int dcIdx = pdcpDcMux->gateSize("toRxEntity");
            pdcpDcMux->setGateSize("toRxEntity", dcIdx + 1);
            pdcpDcMux->gate("toRxEntity", dcIdx)->connectTo(module->gate("dcIn"));
        }
        module->scheduleStart(simTime());
        module->callInitialize();
        auto *rxEnt = check_and_cast<PdcpRxEntityBase *>(module);
        (isDcSecondaryDrb ? pdcpRxEntities2_ : pdcpRxEntities_)[id] = rxEnt;
    }
    else {
        // Native EN-DC/X2 bypass path -- unreachable for nascTime's
        // DC-secondary DRBs (same reasoning as createOutgoingConnection).
        // Unchanged from pristine.
        ASSERT(pdcpDcMux != nullptr); // bypass entities are eNB-only
        DrbKey id = DrbKey(lteInfo->getSourceId(), lteInfo->getDrbId());
        std::string name = "pdcp-bypass-rx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpBypassRxEntityModuleType_->create(name.c_str(), nicModule_);
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));
        auto rlcIt2 = (isNrUe(lteInfo->getDestId()) ? nrRlcRxEntities_ : rlcRxEntities_).find(rlcId);
        ASSERT(rlcIt2 != (isNrUe(lteInfo->getDestId()) ? nrRlcRxEntities_ : rlcRxEntities_).end());
        rlcIt2->second->gate("out")->connectTo(module->gate("in"));
        int fromIdx = pdcpDcMux->gateSize("fromEntity");
        pdcpDcMux->setGateSize("fromEntity", fromIdx + 1);
        module->gate("out")->connectTo(pdcpDcMux->gate("fromEntity", fromIdx));
        module->scheduleStart(simTime());
        module->callInitialize();
        auto *rxEnt = check_and_cast<PdcpRxEntityBase *>(module);
        pdcpBypassRxEntities_[id] = rxEnt;
    }
}

void BearerManagement::setRlcEntityParams(cModule *entity, bool isNr, bool isDcSecondary)
{
    // nascTime / FRER: three-way, was two-way. Without isDcSecondary, every
    // RLC entity created for the secondary leg had its internal macModule
    // parameter pointed at "^.nrMac" (the PRIMARY NR MAC) instead of
    // "^.nrMac2" -- silent, not crashing: the secondary leg's RLC would
    // request grants from the wrong MAC/scheduler.
    if (entity->hasPar("macModule"))
        entity->par("macModule").setStringValue(!isNr ? "^.mac" : (isDcSecondary ? "^.nrMac2" : "^.nrMac"));
    if (entity->hasPar("isNR"))
        entity->par("isNR").setBoolValue(isNr);
}

void BearerManagement::setEntityDisplayPosition(cModule *entity, bool isPdcpEntity, cModule *rlcMux, int bearerIndex)
{
    auto *pdcpMux = nicModule_->getSubmodule("pdcpMux");
    if (!pdcpMux || !rlcMux)
        return;

    int uy = atoi(pdcpMux->getDisplayString().getTagArg("p", 1));
    int lx = atoi(rlcMux->getDisplayString().getTagArg("p", 0));
    int ly = atoi(rlcMux->getDisplayString().getTagArg("p", 1));

    int x = lx + 60 * bearerIndex;
    int y = isPdcpEntity ? uy + (ly - uy) / 3 : uy + 2 * (ly - uy) / 3;

    entity->getDisplayString().setTagArg("p", 0, x);
    entity->getDisplayString().setTagArg("p", 1, y);
}

RlcTxEntityBase *BearerManagement::createAndInstallRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr)
{
    LteRlcType rlcType = static_cast<LteRlcType>(lteInfo->getRlcType());
    cModuleType *moduleType;
    const char *prefix;
    switch (rlcType) {
        case TM: moduleType = rlcTmTxEntityModuleType_; prefix = "tm-tx"; break;
        case AM: moduleType = rlcAmTxEntityModuleType_; prefix = "am-tx"; break;
        default: moduleType = rlcUmTxEntityModuleType_; prefix = "um-tx"; break;
    }
    std::string name = std::string(isNr ? "nrRlc-" : "rlc-") + prefix + "-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
    auto *module = moduleType->create(name.c_str(), nicModule_);
    // nascTime / FRER: same rlcMux-identity check already used below for
    // registry selection -- single source of truth, not a second parameter
    // that could drift out of sync with it.
    bool isDcSecondary = (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get());
    setRlcEntityParams(module, isNr, isDcSecondary);
    module->finalizeParameters();
    module->buildInside();
    setEntityDisplayPosition(module, false, rlcMux, num(id.getDrbId()));

    int fromIdx = rlcMux->gateSize("fromTxEntity");
    rlcMux->setGateSize("fromTxEntity", fromIdx + 1);
    module->gate("out")->connectTo(rlcMux->gate("fromTxEntity", fromIdx));

    int macIdx = rlcMux->gateSize("macToTxEntity");
    rlcMux->setGateSize("macToTxEntity", macIdx + 1);
    rlcMux->gate("macToTxEntity", macIdx)->connectTo(module->gate("macIn"));

    module->scheduleStart(simTime());
    module->callInitialize();

    RlcTxEntityBase *txEnt = check_and_cast<RlcTxEntityBase *>(module);
    txEnt->setFlowControlInfo(lteInfo);

    // nascTime / FRER: registry selection now needs to distinguish
    // NR_SECONDARY from NR_PRIMARY, not just isNr. Since this helper only
    // receives a bool, the caller (createOutgoingConnection) picks the
    // right registry itself after this call returns for TX; this function
    // still writes to the legacy two registries by rlcMux identity, which
    // is safe because rlcMux is already leg-specific by the time it's
    // passed in -- but the registry write below needs the SAME leg
    // information the caller already resolved. Simplest correct fix:
    // compare rlcMux against the known secondary mux pointer directly.
    if (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get())
        nrRlcTxEntities2_[id] = txEnt;
    else
        (isNr ? nrRlcTxEntities_ : rlcTxEntities_)[id] = txEnt;

    if (rlcType == UM) {
        auto *d2dCtrl = dynamic_cast<D2DModeController *>(nicModule_->getSubmodule("rrc")->getSubmodule("d2dModeController"));
        if (d2dCtrl) {
            auto *umTxEnt = check_and_cast<UmTxEntity *>(txEnt);
            d2dCtrl->registerD2DPeerTxEntity(MacNodeId(lteInfo->getD2dRxPeerId()), umTxEnt);
        }
    }

    return txEnt;
}

RlcRxEntityBase *BearerManagement::createAndInstallRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr)
{
    LteRlcType rlcType = static_cast<LteRlcType>(lteInfo->getRlcType());
    cModuleType *moduleType;
    const char *prefix;
    switch (rlcType) {
        case TM: moduleType = rlcTmRxEntityModuleType_; prefix = "tm-rx"; break;
        case AM: moduleType = rlcAmRxEntityModuleType_; prefix = "am-rx"; break;
        default: moduleType = rlcUmRxEntityModuleType_; prefix = "um-rx"; break;
    }
    std::string name = std::string(isNr ? "nrRlc-" : "rlc-") + prefix + "-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
    auto *module = moduleType->create(name.c_str(), nicModule_);
    // nascTime / FRER: same rlcMux-identity check already used below for
    // registry selection -- single source of truth, not a second parameter
    // that could drift out of sync with it.
    bool isDcSecondary = (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get());
    setRlcEntityParams(module, isNr, isDcSecondary);
    module->finalizeParameters();
    module->buildInside();
    setEntityDisplayPosition(module, false, rlcMux, num(id.getDrbId()));

    int idx = rlcMux->gateSize("toRxEntity");
    rlcMux->setGateSize("toRxEntity", idx + 1);
    rlcMux->gate("toRxEntity", idx)->connectTo(module->gate("in"));

    module->scheduleStart(simTime());
    module->callInitialize();

    RlcRxEntityBase *rxEnt = check_and_cast<RlcRxEntityBase *>(module);
    rxEnt->setFlowControlInfo(lteInfo);

    rlcMux->registerRxBuffer(id, rxEnt);

    // nascTime / FRER: same registry-selection note as createAndInstallRlcTxBuffer.
    if (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get())
        nrRlcRxEntities2_[id] = rxEnt;
    else
        (isNr ? nrRlcRxEntities_ : rlcRxEntities_)[id] = rxEnt;

    return rxEnt;
}

RlcTxEntityBase *BearerManagement::createRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo)
{
    Enter_Method_Silent("createRlcTxBuffer()");
    return createAndInstallRlcTxBuffer(id, lteInfo, rlcMuxModule.get(), false);
}

RlcRxEntityBase *BearerManagement::createRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo)
{
    Enter_Method_Silent("createRlcRxBuffer()");
    return createAndInstallRlcRxBuffer(id, lteInfo, rlcMuxModule.get(), false);
}

void BearerManagement::deleteLocalPdcpEntities(MacNodeId nodeId, RlcLeg leg)
{
    Enter_Method_Silent("deleteLocalPdcpEntities()");

    bool isEnb = (registration_->getNodeType() == NODEB);

    // nascTime / FRER FIX: previously "isEnb ? filter-by-nodeId : true" --
    // the UE branch deleted ALL entities unconditionally, regardless of
    // nodeId. That's harmless for a UE with exactly one gNB per leg (the
    // old assumption) but would silently wipe out an unrelated,
    // still-attached leg's entities the first time ANY leg independently
    // handed over or detached. Now always filters by nodeId, for both
    // node types.
    auto& txMap = (leg == RlcLeg::NR_SECONDARY) ? pdcpTxEntities2_ : pdcpTxEntities_;
    auto& rxMap = (leg == RlcLeg::NR_SECONDARY) ? pdcpRxEntities2_ : pdcpRxEntities_;

    auto *pdcpMux = check_and_cast<UpperMux *>(
        nicModule_->getSubmodule(leg == RlcLeg::NR_SECONDARY ? "pdcpMux2" : "pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux"));

    for (auto it = txMap.begin(); it != txMap.end(); ) {
        if (it->first.getNodeId() == nodeId) {
            pdcpMux->unregisterTxEntity(it->first);
            it->second->deleteModule();
            it = txMap.erase(it);
        } else ++it;
    }

    for (auto it = rxMap.begin(); it != rxMap.end(); ) {
        if (it->first.getNodeId() == nodeId) {
            it->second->deleteModule();
            it = rxMap.erase(it);
        } else ++it;
    }

    // Bypass entities are native-DC/X2-only, never used by NR_SECONDARY.
    if (leg != RlcLeg::NR_SECONDARY) {
        ASSERT(pdcpBypassTxEntities_.empty() || pdcpDcMux != nullptr);
        for (auto it = pdcpBypassTxEntities_.begin(); it != pdcpBypassTxEntities_.end(); ) {
            if (it->first.getNodeId() == nodeId) {
                pdcpDcMux->unregisterBypassTxEntity(it->first);
                it->second->deleteModule();
                it = pdcpBypassTxEntities_.erase(it);
            } else ++it;
        }
        for (auto it = pdcpBypassRxEntities_.begin(); it != pdcpBypassRxEntities_.end(); ) {
            if (it->first.getNodeId() == nodeId) {
                it->second->deleteModule();
                it = pdcpBypassRxEntities_.erase(it);
            } else ++it;
        }
    }
}

void BearerManagement::deleteLocalRlcQueues(MacNodeId nodeId, RlcLeg leg)
{
    Enter_Method_Silent("deleteLocalRlcQueues()");

    bool isEnb = (registration_->getNodeType() == NODEB);

    // At a NODEB, entities are always stored in the LTE-named maps
    // regardless of the caller's leg: createIncoming/OutgoingConnection()
    // computes isNr as (nodeType==UE && ...), always false at a gNB, and
    // gnb2 (per the confirmed design) has no second NR leg of its own --
    // it's an ordinary single-NR-stack gNB from its own perspective.
    RlcLeg effectiveLeg = isEnb ? RlcLeg::LTE : leg;

    auto& txMap = (effectiveLeg == RlcLeg::NR_SECONDARY) ? nrRlcTxEntities2_
                : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? nrRlcTxEntities_
                                                          : rlcTxEntities_;
    auto& rxMap = (effectiveLeg == RlcLeg::NR_SECONDARY) ? nrRlcRxEntities2_
                : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? nrRlcRxEntities_
                                                          : rlcRxEntities_;
    RlcMux *rlcMux = (effectiveLeg == RlcLeg::NR_SECONDARY) ? (nrRlcMuxModule2 ? nrRlcMuxModule2.get() : nullptr)
                    : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? (nrRlcMuxModule ? nrRlcMuxModule.get() : nullptr)
                                                              : rlcMuxModule.get();
    if (!rlcMux)
        return;

    for (auto it = txMap.begin(); it != txMap.end(); ) {
        if (isEnb ? it->first.getNodeId() == nodeId : true) {
            it->second->deleteModule();
            it = txMap.erase(it);
        } else ++it;
    }

    for (auto it = rxMap.begin(); it != rxMap.end(); ) {
        if (isEnb ? it->first.getNodeId() == nodeId : true) {
            rlcMux->unregisterRxBuffer(it->first);
            it->second->deleteModule();
            it = rxMap.erase(it);
        } else ++it;
    }
}

PdcpRxEntityBase *BearerManagement::lookupPdcpRxEntity(DrbKey id)
{
    auto it = pdcpRxEntities_.find(id);
    if (it != pdcpRxEntities_.end())
        return it->second;
    auto it2 = pdcpRxEntities2_.find(id);
    if (it2 != pdcpRxEntities2_.end())
        return it2->second;
    auto it3 = pdcpBypassRxEntities_.find(id);
    return it3 != pdcpBypassRxEntities_.end() ? it3->second : nullptr;
}

RlcTxEntityBase *BearerManagement::lookupRlcTxBuffer(DrbKey id)
{
    auto it = rlcTxEntities_.find(id);
    if (it != rlcTxEntities_.end())
        return it->second;
    auto it2 = nrRlcTxEntities_.find(id);
    if (it2 != nrRlcTxEntities_.end())
        return it2->second;
    auto it3 = nrRlcTxEntities2_.find(id);
    return it3 != nrRlcTxEntities2_.end() ? it3->second : nullptr;
}

PdcpTxEntityBase *BearerManagement::lookupPdcpTxEntity(DrbKey id)
{
    auto it = pdcpTxEntities_.find(id);
    if (it != pdcpTxEntities_.end())
        return it->second;
    auto it2 = pdcpTxEntities2_.find(id);
    return it2 != pdcpTxEntities2_.end() ? it2->second : nullptr;
}

void BearerManagement::pdcpActiveUeUL(std::set<MacNodeId> *ueSet)
{
    for (const auto& [id, rxEntity] : pdcpRxEntities_) {
        if (!rxEntity->isEmpty())
            ueSet->insert(id.getNodeId());
    }
    for (const auto& [id, rxEntity] : pdcpRxEntities2_) {
        if (!rxEntity->isEmpty())
            ueSet->insert(id.getNodeId());
    }
}

} // namespace simu5g
