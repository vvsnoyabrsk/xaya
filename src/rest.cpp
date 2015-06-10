// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "main.h"
#include "names/common.h"
#include "rpcserver.h"
#include "streams.h"
#include "sync.h"
#include "txmempool.h"
#include "utilstrencodings.h"
#include "version.h"

#include <boost/algorithm/string.hpp>
#include <boost/dynamic_bitset.hpp>

#include "univalue/univalue.h"

using namespace std;

static const int MAX_GETUTXOS_OUTPOINTS = 15; //allow a max of 15 outpoints to be queried at once

enum RetFormat {
    RF_UNDEF,
    RF_BINARY,
    RF_HEX,
    RF_JSON,
};

static const struct {
    enum RetFormat rf;
    const char* name;
} rf_names[] = {
      {RF_UNDEF, ""},
      {RF_BINARY, "bin"},
      {RF_HEX, "hex"},
      {RF_JSON, "json"},
};

struct CCoin {
    uint32_t nTxVer; // Don't call this nVersion, that name has a special meaning inside IMPLEMENT_SERIALIZE
    uint32_t nHeight;
    CTxOut out;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nTxVer);
        READWRITE(nHeight);
        READWRITE(out);
    }
};

class RestErr
{
public:
    enum HTTPStatusCode status;
    string message;
};

extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry);
extern UniValue blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool txDetails = false);
extern void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex);

static RestErr RESTERR(enum HTTPStatusCode status, string message)
{
    RestErr re;
    re.status = status;
    re.message = message;
    return re;
}

static enum RetFormat ParseDataFormat(std::string& param, const std::string& strReq)
{
    const std::string::size_type pos = strReq.rfind('.');
    if (pos == std::string::npos)
    {
        param = strReq;
        return rf_names[0].rf;
    }

    param = strReq.substr(0, pos);
    const std::string suff(strReq, pos + 1);

    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (suff == rf_names[i].name)
            return rf_names[i].rf;
    return rf_names[0].rf;
}

static string AvailableDataFormatsString()
{
    string formats = "";
    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (strlen(rf_names[i].name) > 0) {
            formats.append(".");
            formats.append(rf_names[i].name);
            formats.append(", ");
        }

    if (formats.length() > 0)
        return formats.substr(0, formats.length() - 2);

    return formats;
}

static bool ParseHashStr(const string& strReq, uint256& v)
{
    if (!IsHex(strReq) || (strReq.size() != 64))
        return false;

    v.SetHex(strReq);
    return true;
}

static bool DecodeName(valtype& decoded, const std::string& encoded)
{
    decoded.clear();
    for (std::string::const_iterator i = encoded.begin(); i != encoded.end(); ++i)
    {
        switch (*i)
        {
        case '+':
            decoded.push_back(' ');
            continue;

        case '%':
        {
            if (i + 2 >= encoded.end())
                return false;
            const std::string hexStr(i + 1, i + 3);
            i += 2;

            int intChar = 0;
            BOOST_FOREACH(char c, hexStr)
            {
                intChar <<= 4;

                if (c >= '0' && c <= '9')
                    intChar += c - '0';
                else
                {
                    c |= (1 << 5);
                    if (c >= 'a' && c <= 'f')
                        intChar += c - 'a' + 10;
                    else
                        return false;
                }
            }

            decoded.push_back(static_cast<char>(intChar));
            continue;
        }

        default:
            decoded.push_back(*i);
            continue;
        }
    }

    return true;
}

static bool rest_headers(AcceptedConnection* conn,
                         const std::string& strURIPart,
                         const std::string& strRequest,
                         const std::map<std::string, std::string>& mapHeaders,
                         bool fRun)
{
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);
    vector<string> path;
    boost::split(path, param, boost::is_any_of("/"));

    if (path.size() != 2)
        throw RESTERR(HTTP_BAD_REQUEST, "No header count specified. Use /rest/headers/<count>/<hash>.<ext>.");

    long count = strtol(path[0].c_str(), NULL, 10);
    if (count < 1 || count > 2000)
        throw RESTERR(HTTP_BAD_REQUEST, "Header count out of range: " + path[0]);

    string hashStr = path[1];
    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        throw RESTERR(HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    std::vector<CBlockHeader> headers;
    headers.reserve(count);
    {
        LOCK(cs_main);
        BlockMap::const_iterator it = mapBlockIndex.find(hash);
        const CBlockIndex *pindex = (it != mapBlockIndex.end()) ? it->second : NULL;
        while (pindex != NULL && chainActive.Contains(pindex)) {
            headers.push_back(pindex->GetBlockHeader());
            if (headers.size() == (unsigned long)count)
                break;
            pindex = chainActive.Next(pindex);
        }
    }

    CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
    BOOST_FOREACH(const CBlockHeader &header, headers) {
        ssHeader << header;
    }

    switch (rf) {
    case RF_BINARY: {
        string binaryHeader = ssHeader.str();
        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, binaryHeader.size(), "application/octet-stream") << binaryHeader << std::flush;
        return true;
    }

    case RF_HEX: {
        string strHex = HexStr(ssHeader.begin(), ssHeader.end()) + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strHex, fRun, false, "text/plain") << std::flush;
        return true;
    }

    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: .bin, .hex)");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool rest_block(AcceptedConnection* conn,
                       const std::string& strURIPart,
                       const std::string& strRequest,
                       const std::map<std::string, std::string>& mapHeaders,
                       bool fRun,
                       bool showTxDetails)
{
    std::string hashStr;
    const RetFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        throw RESTERR(HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    CBlock block;
    CBlockIndex* pblockindex = NULL;
    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hash) == 0)
            throw RESTERR(HTTP_NOT_FOUND, hashStr + " not found");

        pblockindex = mapBlockIndex[hash];
        if (fHavePruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0)
            throw RESTERR(HTTP_NOT_FOUND, hashStr + " not available (pruned data)");

        if (!ReadBlockFromDisk(block, pblockindex))
            throw RESTERR(HTTP_NOT_FOUND, hashStr + " not found");
    }

    CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
    ssBlock << block;

    switch (rf) {
    case RF_BINARY: {
        string binaryBlock = ssBlock.str();
        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, binaryBlock.size(), "application/octet-stream") << binaryBlock << std::flush;
        return true;
    }

    case RF_HEX: {
        string strHex = HexStr(ssBlock.begin(), ssBlock.end()) + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strHex, fRun, false, "text/plain") << std::flush;
        return true;
    }

    case RF_JSON: {
        UniValue objBlock = blockToJSON(block, pblockindex, showTxDetails);
        string strJSON = objBlock.write() + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strJSON, fRun) << std::flush;
        return true;
    }

    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool rest_block_extended(AcceptedConnection* conn,
                       const std::string& strURIPart,
                       const std::string& strRequest,
                       const std::map<std::string, std::string>& mapHeaders,
                       bool fRun)
{
    return rest_block(conn, strURIPart, strRequest, mapHeaders, fRun, true);
}

static bool rest_block_notxdetails(AcceptedConnection* conn,
                       const std::string& strURIPart,
                       const std::string& strRequest,
                       const std::map<std::string, std::string>& mapHeaders,
                       bool fRun)
{
    return rest_block(conn, strURIPart, strRequest, mapHeaders, fRun, false);
}

static bool rest_chaininfo(AcceptedConnection* conn,
                           const std::string& strURIPart,
                           const std::string& strRequest,
                           const std::map<std::string, std::string>& mapHeaders,
                           bool fRun)
{
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RF_JSON: {
        UniValue rpcParams(UniValue::VARR);
        UniValue chainInfoObject = getblockchaininfo(rpcParams, false);
        string strJSON = chainInfoObject.write() + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strJSON, fRun) << std::flush;
        return true;
    }
    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool rest_tx(AcceptedConnection* conn,
                    const std::string& strURIPart,
                    const std::string& strRequest,
                    const std::map<std::string, std::string>& mapHeaders,
                    bool fRun)
{
    std::string hashStr;
    const RetFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        throw RESTERR(HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    CTransaction tx;
    uint256 hashBlock = uint256();
    if (!GetTransaction(hash, tx, hashBlock, true))
        throw RESTERR(HTTP_NOT_FOUND, hashStr + " not found");

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;

    switch (rf) {
    case RF_BINARY: {
        string binaryTx = ssTx.str();
        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, binaryTx.size(), "application/octet-stream") << binaryTx << std::flush;
        return true;
    }

    case RF_HEX: {
        string strHex = HexStr(ssTx.begin(), ssTx.end()) + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strHex, fRun, false, "text/plain") << std::flush;
        return true;
    }

    case RF_JSON: {
        UniValue objTx(UniValue::VOBJ);
        TxToJSON(tx, hashBlock, objTx);
        string strJSON = objTx.write() + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strJSON, fRun) << std::flush;
        return true;
    }

    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool rest_getutxos(AcceptedConnection* conn,
                          const std::string& strURIPart,
                          const std::string& strRequest,
                          const std::map<std::string, std::string>& mapHeaders,
                          bool fRun)
{
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    vector<string> uriParts;
    if (param.length() > 1)
    {
        std::string strUriParams = param.substr(1);
        boost::split(uriParts, strUriParams, boost::is_any_of("/"));
    }

    // throw exception in case of a empty request
    if (strRequest.length() == 0 && uriParts.size() == 0)
        throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Error: empty request");

    bool fInputParsed = false;
    bool fCheckMemPool = false;
    vector<COutPoint> vOutPoints;

    // parse/deserialize input
    // input-format = output-format, rest/getutxos/bin requires binary input, gives binary output, ...

    if (uriParts.size() > 0)
    {

        //inputs is sent over URI scheme (/rest/getutxos/checkmempool/txid1-n/txid2-n/...)
        if (uriParts.size() > 0 && uriParts[0] == "checkmempool")
            fCheckMemPool = true;

        for (size_t i = (fCheckMemPool) ? 1 : 0; i < uriParts.size(); i++)
        {
            uint256 txid;
            int32_t nOutput;
            std::string strTxid = uriParts[i].substr(0, uriParts[i].find("-"));
            std::string strOutput = uriParts[i].substr(uriParts[i].find("-")+1);

            if (!ParseInt32(strOutput, &nOutput) || !IsHex(strTxid))
                throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Parse error");

            txid.SetHex(strTxid);
            vOutPoints.push_back(COutPoint(txid, (uint32_t)nOutput));
        }

        if (vOutPoints.size() > 0)
            fInputParsed = true;
        else
            throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Error: empty request");
    }

    string strRequestMutable = strRequest; //convert const string to string for allowing hex to bin converting

    switch (rf) {
    case RF_HEX: {
        // convert hex to bin, continue then with bin part
        std::vector<unsigned char> strRequestV = ParseHex(strRequest);
        strRequestMutable.assign(strRequestV.begin(), strRequestV.end());
    }

    case RF_BINARY: {
        try {
            //deserialize only if user sent a request
            if (strRequestMutable.size() > 0)
            {
                if (fInputParsed) //don't allow sending input over URI and HTTP RAW DATA
                    throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Combination of URI scheme inputs and raw post data is not allowed");

                CDataStream oss(SER_NETWORK, PROTOCOL_VERSION);
                oss << strRequestMutable;
                oss >> fCheckMemPool;
                oss >> vOutPoints;
            }
        } catch (const std::ios_base::failure& e) {
            // abort in case of unreadable binary data
            throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Parse error");
        }
        break;
    }

    case RF_JSON: {
        if (!fInputParsed)
            throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, "Error: empty request");
        break;
    }
    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // limit max outpoints
    if (vOutPoints.size() > MAX_GETUTXOS_OUTPOINTS)
        throw RESTERR(HTTP_INTERNAL_SERVER_ERROR, strprintf("Error: max outpoints exceeded (max: %d, tried: %d)", MAX_GETUTXOS_OUTPOINTS, vOutPoints.size()));

    // check spentness and form a bitmap (as well as a JSON capable human-readble string representation)
    vector<unsigned char> bitmap;
    vector<CCoin> outs;
    std::string bitmapStringRepresentation;
    boost::dynamic_bitset<unsigned char> hits(vOutPoints.size());
    {
        LOCK2(cs_main, mempool.cs);

        CCoinsView viewDummy;
        CCoinsViewCache view(&viewDummy);

        CCoinsViewCache& viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(&viewChain, mempool);

        if (fCheckMemPool)
            view.SetBackend(viewMempool); // switch cache backend to db+mempool in case user likes to query mempool

        for (size_t i = 0; i < vOutPoints.size(); i++) {
            CCoins coins;
            uint256 hash = vOutPoints[i].hash;
            if (view.GetCoins(hash, coins)) {
                mempool.pruneSpent(hash, coins);
                if (coins.IsAvailable(vOutPoints[i].n)) {
                    hits[i] = true;
                    // Safe to index into vout here because IsAvailable checked if it's off the end of the array, or if
                    // n is valid but points to an already spent output (IsNull).
                    CCoin coin;
                    coin.nTxVer = coins.nVersion;
                    coin.nHeight = coins.nHeight;
                    coin.out = coins.vout.at(vOutPoints[i].n);
                    assert(!coin.out.IsNull());
                    outs.push_back(coin);
                }
            }

            bitmapStringRepresentation.append(hits[i] ? "1" : "0"); // form a binary string representation (human-readable for json output)
        }
    }
    boost::to_block_range(hits, std::back_inserter(bitmap));

    switch (rf) {
    case RF_BINARY: {
        // serialize data
        // use exact same output as mentioned in Bip64
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << chainActive.Height() << chainActive.Tip()->GetBlockHash() << bitmap << outs;
        string ssGetUTXOResponseString = ssGetUTXOResponse.str();

        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, ssGetUTXOResponseString.size(), "application/octet-stream") << ssGetUTXOResponseString << std::flush;
        return true;
    }

    case RF_HEX: {
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << chainActive.Height() << chainActive.Tip()->GetBlockHash() << bitmap << outs;
        string strHex = HexStr(ssGetUTXOResponse.begin(), ssGetUTXOResponse.end()) + "\n";

        conn->stream() << HTTPReply(HTTP_OK, strHex, fRun, false, "text/plain") << std::flush;
        return true;
    }

    case RF_JSON: {
        UniValue objGetUTXOResponse(UniValue::VOBJ);

        // pack in some essentials
        // use more or less the same output as mentioned in Bip64
        objGetUTXOResponse.push_back(Pair("chainHeight", chainActive.Height()));
        objGetUTXOResponse.push_back(Pair("chaintipHash", chainActive.Tip()->GetBlockHash().GetHex()));
        objGetUTXOResponse.push_back(Pair("bitmap", bitmapStringRepresentation));

        UniValue utxos(UniValue::VARR);
        BOOST_FOREACH (const CCoin& coin, outs) {
            UniValue utxo(UniValue::VOBJ);
            utxo.push_back(Pair("txvers", (int32_t)coin.nTxVer));
            utxo.push_back(Pair("height", (int32_t)coin.nHeight));
            utxo.push_back(Pair("value", ValueFromAmount(coin.out.nValue)));

            // include the script in a json output
            UniValue o(UniValue::VOBJ);
            ScriptPubKeyToJSON(coin.out.scriptPubKey, o, true);
            utxo.push_back(Pair("scriptPubKey", o));
            utxos.push_back(utxo);
        }
        objGetUTXOResponse.push_back(Pair("utxos", utxos));

        // return json string
        string strJSON = objGetUTXOResponse.write() + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strJSON, fRun) << std::flush;
        return true;
    }
    default: {
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static bool rest_name(AcceptedConnection* conn,
                      const std::string& strURIPart,
                      const std::string& strRequest,
                      const std::map<std::string, std::string>& mapHeaders,
                      bool fRun)
{
    std::string encodedName;
    const RetFormat rf = ParseDataFormat(encodedName, strURIPart);

    valtype plainName;
    if (!DecodeName(plainName, encodedName))
        throw RESTERR(HTTP_BAD_REQUEST, "Invalid encoded name: " + encodedName);

    CNameData data;
    if (!pcoinsTip->GetName(plainName, data))
        throw RESTERR(HTTP_NOT_FOUND, "'" + ValtypeToString(plainName) + "' not found");

    switch (rf)
    {
    case RF_BINARY:
    {
        const std::string binVal = ValtypeToString(data.getValue());
        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, binVal.size(),
                                          "text/plain")
                       << binVal << std::flush;
        return true;
    }

    case RF_HEX:
    {
        const valtype& binVal = data.getValue();
        const std::string hexVal = HexStr(binVal.begin(), binVal.end()) + "\n";
        conn->stream() << HTTPReply(HTTP_OK, hexVal, fRun, false, "text/plain")
                       << std::flush;
        return true;
    }

    case RF_JSON:
    {
        const UniValue obj = getNameInfo(plainName, data);
        const std::string strJSON = obj.write() + "\n";
        conn->stream() << HTTPReply(HTTP_OK, strJSON, fRun) << std::flush;
        return true;
    }

    default:
        throw RESTERR(HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}

static const struct {
    const char* prefix;
    bool (*handler)(AcceptedConnection* conn,
                    const std::string& strURIPart,
                    const std::string& strRequest,
                    const std::map<std::string, std::string>& mapHeaders,
                    bool fRun);
} uri_prefixes[] = {
      {"/rest/tx/", rest_tx},
      {"/rest/block/notxdetails/", rest_block_notxdetails},
      {"/rest/block/", rest_block_extended},
      {"/rest/chaininfo", rest_chaininfo},
      {"/rest/headers/", rest_headers},
      {"/rest/getutxos", rest_getutxos},
      {"/rest/name/", rest_name},
};

bool HTTPReq_REST(AcceptedConnection* conn,
                  const std::string& strURI,
                  const string& strRequest,
                  const std::map<std::string, std::string>& mapHeaders,
                  bool fRun)
{
    try {
        std::string statusmessage;
        if (RPCIsInWarmup(&statusmessage))
            throw RESTERR(HTTP_SERVICE_UNAVAILABLE, "Service temporarily unavailable: " + statusmessage);

        for (unsigned int i = 0; i < ARRAYLEN(uri_prefixes); i++) {
            unsigned int plen = strlen(uri_prefixes[i].prefix);
            if (strURI.substr(0, plen) == uri_prefixes[i].prefix) {
                string strURIPart = strURI.substr(plen);
                return uri_prefixes[i].handler(conn, strURIPart, strRequest, mapHeaders, fRun);
            }
        }
    } catch (const RestErr& re) {
        conn->stream() << HTTPReply(re.status, re.message + "\r\n", false, false, "text/plain") << std::flush;
        return false;
    }

    conn->stream() << HTTPError(HTTP_NOT_FOUND, false) << std::flush;
    return false;
}
