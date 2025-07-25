#include <stdio.h>
#include <boost/asio.hpp>
#include <wi-ssl-helper.hpp>
#include <string>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <core-all.hpp>

namespace wi::core::security
{
    std::string const& wi_ssl_helper::private_key() const
    {
        return m_private_key;
    }
    std::string const& wi_ssl_helper::public_key() const
    {
        return m_public_key;
    }
    std::string const& wi_ssl_helper::certificate() const
    {
        return m_certificate;
    }
    bool wi_ssl_helper::generate_temporary_keys()
    {
        EVP_PKEY_CTX *ctx;
        EVP_PKEY *pkey = nullptr;
        ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (nullptr != ctx)
        {
            if (EVP_PKEY_keygen_init(ctx) > 0)
            {
                if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0)
                {
                    if (EVP_PKEY_keygen(ctx, &pkey) > 0)
                    {
                        BIO* bioPrivate = BIO_new(BIO_s_mem());
                        if (PEM_write_bio_PrivateKey(bioPrivate, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1)
                        {
                            char* private_key_text;
                            BIO_ctrl(bioPrivate, BIO_CTRL_INFO, 0, &private_key_text);
                            BIO* bioPublic = BIO_new(BIO_s_mem());
                            PEM_write_bio_PUBKEY(bioPublic, pkey);
                            char* public_key_text;
                            BIO_ctrl(bioPublic, BIO_CTRL_INFO, 0, &public_key_text);

                            m_private_key = private_key_text;
                            m_public_key = public_key_text;

                            X509* cert = X509_new();
                            X509_gmtime_adj(X509_get_notBefore(cert), 0);
                            int daysValid = 20000;
                            X509_gmtime_adj(X509_get_notAfter(cert), daysValid * 24 * 3600);

                            X509_set_pubkey(cert, pkey);

                            X509_name_st* name = X509_get_subject_name(cert);

                            const auto country = WI_CONFIGURATION().read_settings<std::string>(server_cert_country);
                            const auto location = WI_CONFIGURATION().read_settings<std::string>(server_cert_location);
                            const auto organization = WI_CONFIGURATION().read_settings<std::string>(server_cert_organization);
                            const auto common = boost::asio::ip::host_name();

                            const auto* country_name = reinterpret_cast<const unsigned char*>(country.c_str());
                            const auto* location_name = reinterpret_cast<const unsigned char*>(location.c_str());
                            const auto* organization_name = reinterpret_cast<const unsigned char*>(organization.c_str());
                            const auto* common_name = reinterpret_cast<const unsigned char*>(common.c_str());

                            X509_NAME_add_entry_by_txt(name, "C",  MBSTRING_ASC, country_name, -1, -1, 0);
                            X509_NAME_add_entry_by_txt(name, "L",  MBSTRING_ASC, location_name, -1, -1, 0);
                            X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC, organization_name, -1, -1, 0);
                            X509_NAME_add_entry_by_txt(name, "CN",  MBSTRING_ASC, common_name, -1, -1, 0);

                            X509_set_issuer_name(cert, name);

                            X509_sign(cert, pkey, EVP_sha256());

                            BIO* bio_cert = BIO_new(BIO_s_mem());
                            PEM_write_bio_X509(bio_cert, cert);
                            char* cert_text;
                            BIO_ctrl(bio_cert, BIO_CTRL_INFO, 0, &cert_text);
                            m_certificate = cert_text;

                            X509_free(cert);
                            BIO_free(bio_cert);
                            BIO_free(bioPrivate);
                            BIO_free(bioPublic);

                            return true;
                        }
                        else
                            return false;
                    }
                    else
                        return false;
                }
                else
                    return false;
            }
            else
                return false;
        }
        else
            return false;
    }

    int wi_ssl_helper::calc_sha256(const char* path, std::string &out) {
        FILE *file = fopen(path, "rb");
        if(!file) return -534;
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        const int bufSize = 32768;
        auto *buffer = (std::uint8_t*) malloc(bufSize);
        size_t bytesRead = 0;
        if(!buffer) return ENOMEM;
        while((bytesRead = fread(buffer, 1, bufSize, file))) {
            SHA256_Update(&sha256, buffer, bytesRead);
        }
        SHA256_Final(hash, &sha256);
        char outputBuffer[SHA256_DIGEST_LENGTH * 2 +1];
        outputBuffer[SHA256_DIGEST_LENGTH * 2] = 0;
        for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
        }
        out = outputBuffer;
        fclose(file);
        free(buffer);
        return 0;
    }
}
