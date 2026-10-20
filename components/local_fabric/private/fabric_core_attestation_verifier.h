/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <credentials/DeviceAttestationConstructor.h>
#include <credentials/DeviceAttestationVendorReserved.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>

#include <cstring>

namespace local_fabric {

// Retain device proof and CSR verification without PAA or CD processing.
class paa_cd_bypass_verifier final : public chip::Credentials::DefaultDACVerifier {
public:
    explicit paa_cd_bypass_verifier(
        chip::Credentials::DeviceAttestationRevocationDelegate *revocation_delegate)
        : DefaultDACVerifier(nullptr, revocation_delegate)
    {}

    void VerifyAttestationInformation(
        const AttestationInfo &info,
        chip::Callback::Callback<OnAttestationInformationVerification> *completion) override
    {
        if (completion != nullptr) {
            completion->mCall(completion->mContext, info, verify_device_proof(info));
        }
    }

private:
    chip::Credentials::AttestationVerificationResult verify_device_proof(
        const AttestationInfo &info)
    {
        using namespace chip;
        using namespace chip::Credentials;
        using namespace chip::Crypto;
        using Result = AttestationVerificationResult;

        if (info.attestationElementsBuffer.empty() ||
            info.attestationElementsBuffer.size() > 900 ||
            info.attestationChallengeBuffer.empty() ||
            info.attestationSignatureBuffer.empty() ||
            info.dacDerBuffer.empty() || info.attestationNonceBuffer.empty()) {
            return Result::kInvalidArgument;
        }
        if (info.paiDerBuffer.empty()) return Result::kPaiMissing;
        if (VerifyAttestationCertificateFormat(info.paiDerBuffer, AttestationCertType::kPAI) != CHIP_NO_ERROR) {
            return Result::kPaiFormatInvalid;
        }
        if (VerifyAttestationCertificateFormat(info.dacDerBuffer, AttestationCertType::kDAC) != CHIP_NO_ERROR) {
            return Result::kDacFormatInvalid;
        }

        AttestationCertVidPid dac;
        AttestationCertVidPid pai;
        if (ExtractVIDPIDFromX509Cert(info.dacDerBuffer, dac) != CHIP_NO_ERROR) {
            return Result::kDacFormatInvalid;
        }
        if (ExtractVIDPIDFromX509Cert(info.paiDerBuffer, pai) != CHIP_NO_ERROR) {
            return Result::kPaiFormatInvalid;
        }
        if (!pai.mVendorId.HasValue() || pai.mVendorId != dac.mVendorId) {
            return Result::kDacVendorIdMismatch;
        }
        if (!dac.mProductId.HasValue() ||
            (pai.mProductId.HasValue() && pai.mProductId != dac.mProductId)) {
            return Result::kDacProductIdMismatch;
        }

        P256PublicKey public_key;
        if (ExtractPubkeyFromX509Cert(info.dacDerBuffer, public_key) != CHIP_NO_ERROR) {
            return Result::kDacFormatInvalid;
        }
        P256ECDSASignature signature;
        if (signature.SetLength(info.attestationSignatureBuffer.size()) != CHIP_NO_ERROR) {
            return Result::kAttestationSignatureInvalidFormat;
        }
        std::memcpy(signature.Bytes(), info.attestationSignatureBuffer.data(),
                    info.attestationSignatureBuffer.size());
        if (ValidateAttestationSignature(public_key, info.attestationElementsBuffer,
                                        info.attestationChallengeBuffer, signature) != CHIP_NO_ERROR) {
            return Result::kAttestationSignatureInvalid;
        }

        ByteSpan cd;
        ByteSpan nonce;
        ByteSpan firmware;
        uint32_t timestamp;
        DeviceAttestationVendorReservedDeconstructor vendor_reserved;
        if (DeconstructAttestationElements(info.attestationElementsBuffer, cd, nonce,
                                          timestamp, firmware, vendor_reserved) != CHIP_NO_ERROR) {
            return Result::kAttestationElementsMalformed;
        }
        if (!nonce.data_equal(info.attestationNonceBuffer)) {
            return Result::kAttestationNonceMismatch;
        }
        return Result::kSuccess;
    }
};

} // namespace local_fabric
