// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_NET_X509_CERTIFICATE_MODEL_H_
#define CHROME_COMMON_NET_X509_CERTIFICATE_MODEL_H_

#include <string>
#include <vector>

#include "base/time/time.h"
#include "net/cert/internal/parse_certificate.h"
#include "net/cert/internal/parse_name.h"
#include "third_party/abseil-cpp/absl/types/variant.h"
#include "third_party/boringssl/src/include/openssl/pool.h"

// This namespace defines a set of functions to be used in UI-related bits of
// X509 certificates.
namespace x509_certificate_model {

struct Extension {
  std::string name;
  std::string value;
};

struct NotPresent : absl::monostate {};
struct Error : absl::monostate {};
using OptionalStringOrError = absl::variant<Error, NotPresent, std::string>;

class X509CertificateModel {
 public:
  // Construct an X509CertificateModel from |cert_data|, which must must not be
  // nullptr.  |nickname| may optionally be provided as a platform-specific
  // nickname for the certificate, if available.
  X509CertificateModel(bssl::UniquePtr<CRYPTO_BUFFER> cert_data,
                       std::string nickname);
  ~X509CertificateModel();

  // ---------------------------------------------------------------------------
  // These methods are always safe to call even if |cert_data| could not be
  // parsed.

  // Returns hex SHA256 hash of the certificate data.
  std::string HashCertSHA256() const;
  // Returns space-separated and line wrapped hex SHA256 hash of the
  // certificate data.
  std::string HashCertSHA256WithSeparators() const;
  // Returns space-separated and line wrapped hex SHA1 hash of the certificate
  // data.
  std::string HashCertSHA1WithSeparators() const;

  // Get something that can be used as a title for the certificate, using the
  // following priority:
  //   |nickname| passed to constructor
  //   subject commonName
  //   full subject
  //   dnsName or email address from subjectAltNames
  // If none of those are present, or certificate could not be parsed,
  // the hex SHA256 hash of the certificate data will be returned.
  std::string GetTitle() const;

  CRYPTO_BUFFER* cert_buffer() const { return cert_data_.get(); }
  bool is_valid() const { return parsed_successfully_; }

  // ---------------------------------------------------------------------------
  // The rest of the methods should only be called if |is_valid()| returns true.

  std::string GetVersion() const;
  std::string GetSerialNumberHexified() const;

  // Get the validity notBefore and notAfter times, returning true on success
  // or false on error in parsing or converting to a base::Time.
  bool GetTimes(base::Time* not_before, base::Time* not_after) const;

  // These methods returns the issuer/subject commonName/orgName/orgUnitName
  // formatted as a string, if present. Returns NotPresent if the attribute
  // type was not present, or Error if there was a parsing error.
  // The Get{Issuer,Subject}CommonName methods return the last (most specific)
  // commonName, while the other methods return the first (most general) value.
  // This matches the NSS behaviour of CERT_GetCommonName, CERT_GetOrgName,
  // CERT_GetOrgUnitName.
  OptionalStringOrError GetIssuerCommonName() const;
  OptionalStringOrError GetIssuerOrgName() const;
  OptionalStringOrError GetIssuerOrgUnitName() const;
  OptionalStringOrError GetSubjectCommonName() const;
  OptionalStringOrError GetSubjectOrgName() const;
  OptionalStringOrError GetSubjectOrgUnitName() const;

  // Get the issuer/subject name as a text block with one line per
  // attribute-value pair. Will process IDN in commonName, showing original and
  // decoded forms. Returns NotPresent if the Name was an empty sequence.
  // (Although note that technically an empty issuer name is invalid.)
  OptionalStringOrError GetIssuerName() const;
  OptionalStringOrError GetSubjectName() const;

  // Returns textual representations of the certificate's extensions, if any.
  // |critical_label| and |non_critical_label| will be used in the returned
  // extension.value fields to describe extensions that are critical or
  // non-critical.
  std::vector<Extension> GetExtensions(
      base::StringPiece critical_label,
      base::StringPiece non_critical_label) const;

 private:
  bool ParseExtensions(const net::der::Input& extensions_tlv);
  std::string ProcessExtension(base::StringPiece critical_label,
                               base::StringPiece non_critical_label,
                               const net::ParsedExtension& extension) const;
  absl::optional<std::string> ProcessExtensionData(
      const net::ParsedExtension& extension) const;

  // Externally provided "nickname" for the cert.
  std::string nickname_;

  bool parsed_successfully_ = false;
  bssl::UniquePtr<CRYPTO_BUFFER> cert_data_;
  net::der::Input tbs_certificate_tlv_;
  net::der::Input signature_algorithm_tlv_;
  net::der::BitString signature_value_;
  net::ParsedTbsCertificate tbs_;

  net::RDNSequence subject_rdns_;
  net::RDNSequence issuer_rdns_;
  std::vector<net::ParsedExtension> extensions_;

  // Parsed SubjectAltName extension.
  std::unique_ptr<net::GeneralNames> subject_alt_names_;
};

// For host values, if they contain IDN Punycode-encoded A-labels, this will
// return a string suitable for display that contains both the original and the
// decoded U-label form.  Otherwise, the string will be returned as is.
std::string ProcessIDN(const std::string& input);

// Format a buffer as |hex_separator| separated string, with 16 bytes on each
// line separated using |line_separator|.
std::string ProcessRawBytesWithSeparators(const unsigned char* data,
                                          size_t data_length,
                                          char hex_separator,
                                          char line_separator);

// Format a buffer as a space separated string, with 16 bytes on each line.
std::string ProcessRawBytes(const unsigned char* data, size_t data_length);

// Format a buffer as a space separated string, with 16 bytes on each line.
// |data_length| is the length in bits.
std::string ProcessRawBits(const unsigned char* data, size_t data_length);

}  // namespace x509_certificate_model

#endif  // CHROME_COMMON_NET_X509_CERTIFICATE_MODEL_H_
