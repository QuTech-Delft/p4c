/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef BACKENDS_BMV2_QUANTUM_SWITCH_QUANTUMSWITCH_H_
#define BACKENDS_BMV2_QUANTUM_SWITCH_QUANTUMSWITCH_H_

#include <algorithm>
#include <cstring>
#include "frontends/common/constantFolding.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/fromv1.0/v1model.h"
#include "frontends/p4/simplify.h"
#include "frontends/p4/unusedDeclarations.h"
#include "midend/convertEnums.h"
#include "backends/bmv2/common/action.h"
#include "backends/bmv2/common/backend.h"
#include "backends/bmv2/common/control.h"
#include "backends/bmv2/common/deparser.h"
#include "backends/bmv2/common/extern.h"
#include "backends/bmv2/common/globals.h"
#include "backends/bmv2/common/header.h"
#include "backends/bmv2/common/parser.h"
#include "backends/bmv2/common/programStructure.h"
#include "backends/bmv2/common/sharedActionSelectorCheck.h"
#include "backends/bmv2/common/options.h"

namespace BMV2 {

class V1QProgramStructure : public ProgramStructure {
 public:
    std::set<cstring>                pipeline_controls;
    std::set<cstring>                non_pipeline_controls;

    const IR::P4Parser* parser = nullptr;
    const IR::P4Control* ingress = nullptr;
    const IR::P4Control* qcontrol = nullptr;
    const IR::P4Control* egress = nullptr;
    const IR::P4Control* compute_checksum = nullptr;
    const IR::P4Control* verify_checksum = nullptr;
    const IR::P4Control* deparser = nullptr;

    const cstring parser_name = "p";
    const cstring ingress_name = "ig";
    const cstring qcontrol_name = "qc";
    const cstring egress_name = "eg";
    const cstring compute_checksum_name = "ck";
    const cstring verify_checksum_name = "vr";
    const cstring deparser_name = "dep";

    V1QProgramStructure() { }
};

enum class V1QMetadataType {
    NONE,
    STANDARD,
    QCONTROL,
    XCONNECT,
};

class QuantumSwitchExpressionConverter : public ExpressionConverter {
    V1QProgramStructure* structure;

 public:
    QuantumSwitchExpressionConverter(P4::ReferenceMap* refMap, P4::TypeMap* typeMap,
        V1QProgramStructure* structure, cstring scalarsName) :
        ExpressionConverter(refMap, typeMap, structure, scalarsName), structure(structure) { }

    void modelError(const char* format, const IR::Node* node) {
        ::error(ErrorType::ERR_MODEL,
                (cstring(format) +
                 "\nAre you using an up-to-date v1quantum.p4?").c_str(), node);
    }

    V1QMetadataType getParameterMetadataType(const IR::Parameter* param) {
        auto st = dynamic_cast<V1QProgramStructure*>(structure);
        auto params = st->parser->getApplyParameters();
        if (params->size() != 4) {
            modelError("%1%: Expected 4 parameter for parser", st->parser);
            return V1QMetadataType::NONE;
        }
        if (params->parameters.at(3) == param)
            return V1QMetadataType::STANDARD;

        params = st->ingress->getApplyParameters();
        if (params->size() != 4) {
            modelError("%1%: Expected 4 parameter for ingress", st->ingress);
            return V1QMetadataType::NONE;
        }
        if (params->parameters.at(2) == param)
            return V1QMetadataType::STANDARD;
        else if (params->parameters.at(3) == param)
            return V1QMetadataType::XCONNECT;

        params = st->qcontrol->getApplyParameters();
        if (params->size() != 4) {
            modelError("%1%: Expected 4 parameter for qcontrol", st->qcontrol);
            return V1QMetadataType::NONE;
        }
        if (params->parameters.at(2) == param)
            return V1QMetadataType::QCONTROL;
        else if (params->parameters.at(3) == param)
            return V1QMetadataType::XCONNECT;

        params = st->egress->getApplyParameters();
        if (params->size() != 4) {
            modelError("%1%: Expected 4 parameter for egress", st->egress);
            return V1QMetadataType::NONE;
        }
        if (params->parameters.at(2) == param)
            return V1QMetadataType::STANDARD;
        else if (params->parameters.at(3) == param)
            return V1QMetadataType::XCONNECT;

        return V1QMetadataType::NONE;
    }

    Util::IJson* convertParam(const IR::Parameter* param, cstring fieldName) override {
        V1QMetadataType metadata_type = getParameterMetadataType(param);
        if (metadata_type != V1QMetadataType::NONE) {
            cstring metadata_name;

            switch (metadata_type) {
            case V1QMetadataType::STANDARD:
                metadata_name = "standard_metadata";
                break;
            case V1QMetadataType::QCONTROL:
                metadata_name = "qcontrol_metadata";
                break;
            case V1QMetadataType::XCONNECT:
                metadata_name = "xconnect_metadata";
                break;
            case V1QMetadataType::NONE:
                assert(false);
                break;
            }

            auto result = new Util::JsonObject();
            if (fieldName != "") {
                result->emplace("type", "field");
                auto e = BMV2::mkArrayField(result, "value");
                e->append(metadata_name);
                e->append(fieldName);
            } else {
                result->emplace("type", "header");
                result->emplace("value", metadata_name);
            }

            return result;
        }
        return nullptr;
    }
};

class ParseV1QArchitecture : public Inspector {
    V1QProgramStructure* structure;
    P4V1::V1Model&      v1model;

 public:
    explicit ParseV1QArchitecture(V1QProgramStructure* structure) :
        structure(structure), v1model(P4V1::V1Model::instance) { }
    void modelError(const char* format, const IR::Node* node);
    bool preorder(const IR::PackageBlock* block) override;
};

class V1QHeaderConverter : public HeaderConverter {
    ConversionContext* ctxt;

 protected:
    virtual void addMetadata() override {
        ctxt->json->add_metadata("standard_metadata", "standard_metadata");
        ctxt->json->add_metadata("qcontrol_metadata", "qcontrol_metadata");
        ctxt->json->add_metadata("xconnect_metadata", "xconnect_metadata");
    }

 public:
    V1QHeaderConverter(ConversionContext* ctxt, cstring scalarsName) :
        HeaderConverter(ctxt, scalarsName), ctxt(ctxt) { }
};

class QuantumSwitchBackend : public Backend {
    BMV2Options&                options;
    P4V1::V1Model&              v1model;
    V1QProgramStructure*         structure = nullptr;
    ExpressionConverter*        conv = nullptr;

 protected:
    cstring createCalculation(cstring algo, const IR::Expression* fields,
                              Util::JsonArray* calculations, bool usePayload, const IR::Node* node);

 public:
    void modelError(const char* format, const IR::Node* place) const;
    void convertChecksum(const IR::BlockStatement* body, Util::JsonArray* checksums,
                         Util::JsonArray* calculations, bool verify);
    void createActions(ConversionContext* ctxt, V1QProgramStructure* structure);

    void convert(const IR::ToplevelBlock* tlb) override;
    QuantumSwitchBackend(BMV2Options& options, P4::ReferenceMap* refMap, P4::TypeMap* typeMap,
                         P4::ConvertEnums::EnumMapping* enumMap) :
        Backend(options, refMap, typeMap, enumMap), options(options),
        v1model(P4V1::V1Model::instance) { }
};

EXTERN_CONVERTER_W_FUNCTION(clone)
EXTERN_CONVERTER_W_FUNCTION_AND_MODEL(clone3, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_FUNCTION_AND_MODEL(hash, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_FUNCTION(digest)
EXTERN_CONVERTER_W_FUNCTION(resubmit)
EXTERN_CONVERTER_W_FUNCTION(recirculate)
EXTERN_CONVERTER_W_FUNCTION(mark_to_drop)
EXTERN_CONVERTER_W_FUNCTION(division32)
EXTERN_CONVERTER_W_FUNCTION(log_msg)
EXTERN_CONVERTER_W_FUNCTION_AND_MODEL(random, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_FUNCTION_AND_MODEL(truncate, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_OBJECT_AND_INSTANCE_AND_MODEL(register, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_OBJECT_AND_INSTANCE_AND_MODEL(counter, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_OBJECT_AND_INSTANCE_AND_MODEL(meter, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_OBJECT_AND_INSTANCE(direct_counter)
EXTERN_CONVERTER_W_OBJECT_AND_INSTANCE_AND_MODEL(direct_meter, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_INSTANCE_AND_MODEL(action_profile, P4V1::V1Model, v1model)
EXTERN_CONVERTER_W_INSTANCE_AND_MODEL(action_selector, P4V1::V1Model, v1model)

}  // namespace BMV2

#endif /* BACKENDS_BMV2_QUANTUM_SWITCH_QUANTUMSWITCH_H_ */
