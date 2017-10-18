/*
 * Copyright (C) 2017
 * SUSE Linux GmbH
 * Matthias Gerstner <matthias.gerstner@suse.com>
 */

// C++
#include <iostream>
#include <sstream>
#include <memory>

// Linux
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
// contains a few more preprocessor defines than sys/xattr.h
#include <attr/xattr.h>
#include <time.h>
#include <stddef.h>
#include <arpa/inet.h>

// ima-inspect
#include "ima_inspect.hxx"
#include "ima_exceptions.hxx"
#include "ima_helpers.hxx"

// ima-evm-utils
#include "imaevm.h"

namespace
{

// this is the signature enum used for v1 signatures
const char* getSignAlgoLabel(enum pubkey_algo algo)
{
	switch(algo)
	{
	case PUBKEY_ALGO_RSA: return "rsa";
	default: return "unknown/invalid";
	}
}

// this is the digest enum used for v1 signatures
const char* getDigestAlgoLabel(enum digest_algo algo)
{
	switch(algo)
	{
	case DIGEST_ALGO_SHA1: return "sha1";
	case DIGEST_ALGO_SHA256: return "sha256";
	default: return "unknown/invalid";
	}
}

// this is the digest enum used for v2 signatures
const char* getHashAlgoLabel(enum pkey_hash_algo algo)
{
	// there is actually a translation table in libimaevm.c but it's not
	// declared in the header...

	switch(algo)
	{
	case PKEY_HASH_MD4: return "md4";
	case PKEY_HASH_MD5: return "md5";
	case PKEY_HASH_SHA1: return "sha1";
	case PKEY_HASH_RIPE_MD_160: return "rmd160";
	case PKEY_HASH_SHA224: return "sha224";
	case PKEY_HASH_SHA256: return "sha256";
	case PKEY_HASH_SHA384: return "sha384";
	case PKEY_HASH_SHA512: return "sha512";
	default: return "unknown/invalid";
	}
}

} // end anon ns

const std::array<std::string, 2> ImaInspect::m_attr_names = {
	"security.ima",
	"security.evm"
};

ImaInspect::ImaInspect() :
	m_cmdline("This utility allows to display security.ima and "
		"security.evm extended attributes created by the "
		"'evmctl' utility from ima-evm-utils."),
	m_arg_files("files", "one or more files to inspect", true, "path",
		m_cmdline)
{
}

void ImaInspect::parseArgs(const int argc, const char **argv)
{
	m_cmdline.parse(argc, argv);
}

void ImaInspect::run()
{
	bool first = true;

	for( const auto &file: m_arg_files.getValue() )
	{
		try
		{
			if( first ) first = false; else std::cout << "\n";
			std::cout << file << "\n";
			inspectFile(file);
		}
		catch( const std::exception &ex )
		{
			std::cerr << ex.what() << std::endl;
			m_res = 2;
		}
	}
}

void ImaInspect::inspectFile(const std::string &path)
{
	auto fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);

	if( fd == -1 )
	{
		throw SysError("open");
	}

	for( const auto &attr: m_attr_names )
	{
		try
		{
			std::cout << "\n";
			std::cout << attr << "\n";
			std::cout << std::string(attr.size(), '-') << "\n";
			bool have_attr = getAttr(fd, attr);

			if( !have_attr )
				continue;

			inspectAttr();
		}
		catch( const std::exception &ex )
		{
			std::cerr << ex.what() << std::endl;
			m_res = 2;
		}
	}

	close(fd);
}

bool ImaInspect::getAttr(int fd, const std::string &attr)
{
	m_attr_data_left = 0;
	m_attr_data.clear();
	int res = -1;

	while( true )
	{
		res = fgetxattr(
			fd,
			attr.c_str(),
			m_attr_data.data(),
			m_attr_data.size()
		);

		if( res == -1 )
		{
			if( errno == ERANGE )
			{
				// re-calculate required space
				m_attr_data.clear();
				continue;
			}
			else if( errno == ENOATTR )
			{
				std::cout << "no such attribute\n";
				// no attribute
				return false;
			}

			throw SysError("fgetxattr");
		}
		else if( m_attr_data.empty() )
		{
			const auto length = static_cast<size_t>(res);
			m_attr_data.resize(length);
		}
		else
		{
			break;
		}
	}

	m_attr_data_left = m_attr_data.size();

	return true;
}

const char* ImaInspect::fetchNextData(const size_t bytes, const char *item) const
{
	assertDataLeft(bytes, item);

	auto ret = nextData();

	recordDataConsumed(bytes);

	return ret;
}

void ImaInspect::assertDataLeft(const size_t bytes, const char *purpose) const
{
	if( m_attr_data_left < bytes )
	{
		std::stringstream ss;
		ss << "premature end of attribute data (" << purpose << ")\n";
		throw RuntimeError(ss.str());
	}
}

template<typename T>
void ImaInspect::fetchNextType(T*& out_ptr, const char *label) const
{
	auto char_ptr = fetchNextData(sizeof(T), label);

	out_ptr = reinterpret_cast<T*>(char_ptr);
}

void ImaInspect::inspectAttr() const
{
	const auto type = static_cast<enum evm_ima_xattr_type>(
		*(fetchNextData(1, "xattr_type"))
	);

	switch(type)
	{
	case IMA_XATTR_DIGEST:
		inspectDigest();
		break;
	case IMA_XATTR_DIGEST_NG:
		inspectDigestNg();
		break;
	case EVM_XATTR_HMAC:
		inspectHmac();
		break;
	case EVM_IMA_XATTR_DIGSIG:
		inspectDigsig();
		break;
	default:
		std::cout << "unknown IMA/EVM attribute tagged with "
			<< static_cast<int>(type) << std::endl;
		break;
	}
}

void ImaInspect::inspectDigsig() const
{
	// there is some union missing for accessing the signature version so
	// we need to fiddle with the leading byte which indicates the right
	// struct to use
	assertDataLeft(1, "digsig version");
	uint8_t digsig_version = static_cast<uint8_t>(*nextData());

	std::cout << "digital signature version " << (unsigned)digsig_version
		<< "\n";

	switch(digsig_version)
	{
	case DIGSIG_VERSION_1:
		inspectDigsigV1();
		break;
	case DIGSIG_VERSION_2:
		inspectDigsigV2();
		break;
	// no constant for this "immutable" variant which is also not
	// documented but reached by passing an "-i" parameter to evmctl like
	// this:
	// evmctl sign -k /some/cert.pem --imasig -i /etc/fstab
	case 3:
		// it's the same as V2 otherwise. It seems only to indicate
		// that the HMAC algorithm needs to be different
		inspectDigsigV2();
		break;
	default:
		std::cout << "unknown digital signature version encountered\n";
		break;
	}
}

void ImaInspect::inspectDigsigV1() const
{
	const struct signature_hdr* header;
	fetchNextType(header, "signature_hdr");

	time_t ts = static_cast<time_t>(header->timestamp);
	// NOTE: ctime() seems to include a newline already
	std::cout << "creation time: " << ::ctime(&ts);
	auto sig_algo = static_cast<pubkey_algo>(header->algo);
	std::cout << "signature algorithm: "
		<< getSignAlgoLabel(sig_algo) << "\n";
	auto dig_algo = static_cast<digest_algo>(header->hash);
	std::cout << "digest algorithm: "
		<< getDigestAlgoLabel(dig_algo) << "\n";
	// these are bytes 12 - 19 of the sha1 digest of the binary public key
	// used
	// TODO: does this need to be displayed in big-endian?
	// see calc_keyid_v1() from libimaevm.c
	std::cout << "key-id v1 (gpg compatible): "
		<< HexDumpData(header->keyid, sizeof(header->keyid)) << "\n";
	// the kernel digsig code was taken from the GMP library, thus the
	// nomenclature
	// also see kernel Documentation/digsig.txt
	std::cout << "nmpi (number of multi-precision-integers): "
		<< (size_t)header->nmpi << "\n";

	// following is a header containing the length of the signature in bits
	const uint16_t *mpi_length_be;
	fetchNextType(mpi_length_be, "mpi_length");
	const auto mpi_length_host = ntohs(*mpi_length_be);
	std::cout << "signature length: " << mpi_length_host << " bits\n";

	// shift by 3 (div by 8) gives us the bytes unit again
	const auto mpi_length_bytes = mpi_length_host >> 3;
	const uint8_t *mpi_sig = reinterpret_cast<const uint8_t*>(
		fetchNextData(mpi_length_bytes, "mpi_sig")
	);
	std::cout << "signature data:\n\n"
		<< HexDumpData(mpi_sig, mpi_length_bytes) << "\n";
}

void ImaInspect::inspectDigsigV2() const
{
	const struct signature_v2_hdr* header;
	fetchNextType(header, "signature_v2_hdr");
	auto hash_algo = static_cast<pkey_hash_algo>(header->hash_algo);
	std::cout << "digest algorithm: " << getHashAlgoLabel(hash_algo) << "\n";
	// for V2 the keyid is only 4 bytes long for some reason
	// TODO: does this need to be displayed in big-endian?
	std::cout << "key-id v2 (gpg compatible): "
		<< HexDumpData(
			reinterpret_cast<const uint8_t*>(&header->keyid),
			sizeof(header->keyid)
		)
		<< "\n";

	const auto sig_size_bytes = ntohs(header->sig_size);
	const auto sig_size_bits = sig_size_bytes << 3;
	std::cout << "signature length: " << sig_size_bits << " bits\n";

	const uint8_t *sig_data = reinterpret_cast<const uint8_t*>(
		fetchNextData(sig_size_bytes, "sig")
	);
	std::cout << "signature data:\n\n"
		<< HexDumpData(sig_data, sig_size_bytes) << "\n";
}

void ImaInspect::inspectDigest() const
{
	std::cout << "digest (legacy sha1)" << "\n";
	const auto digest_len = m_attr_data_left;
	std::cout << "digest length: " << digest_len << " bytes\n";
	const uint8_t *digest = reinterpret_cast<const uint8_t*>(
		fetchNextData(digest_len, "digest")
	);
	std::cout << "digest: " << HexDumpData(digest, digest_len) << "\n";
}

void ImaInspect::inspectDigestNg() const
{
	std::cout << "digest (NG)" << "\n";
	const uint8_t *algo;
	fetchNextType(algo, "digest-ng algo");
	const auto algo_enum = static_cast<enum pkey_hash_algo>(*algo);
	std::cout << "digest algorithm: " << getHashAlgoLabel(algo_enum) << "\n";
	const auto digest_len = m_attr_data_left;
	std::cout << "digest length: " << m_attr_data_left << " bytes\n";
	const uint8_t *digest = reinterpret_cast<const uint8_t*>(
		fetchNextData(digest_len, "digest")
	);
	std::cout << "digest: " << HexDumpData(digest, digest_len) << "\n";
}

void ImaInspect::inspectHmac() const
{
	// The EVM HMAC seems still to be fixed at SHA1?
	std::cout << "EVM HMAC\n";
	std::cout << "digest algo: sha1 (fixed)\n";
	const auto digest_len = m_attr_data_left;
	std::cout << "digest length: " << digest_len << " bytes\n";
	const uint8_t *digest = reinterpret_cast<const uint8_t*>(
		fetchNextData(digest_len, "digest")
	);
	std::cout << "digest: " << HexDumpData(digest, digest_len) << "\n";
}

int main(const int argc, const char **argv)
{
	try
	{
		ImaInspect ima_inspect;
		ima_inspect.parseArgs(argc, argv);
		ima_inspect.run();
		return ima_inspect.getRes();
	}
	catch( const std::exception &ex )
	{
		std::cerr << "Failed: " << ex.what() << std::endl;
		return 1;
	}
}

