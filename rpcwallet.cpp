// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/rbf.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <util/bip32.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/string.h>
#include <util/system.h>
#include <util/url.h>
#include <util/validation.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/psbtwallet.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <stdint.h>
#include <deque>

#include <univalue.h>


static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

static inline bool GetAvoidReuseFlag(CWallet * const pwallet, const UniValue& param) {
    bool can_avoid_reuse = pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);
    bool avoid_reuse = param.isNull() ? can_avoid_reuse : param.get_bool();

    if (avoid_reuse && !can_avoid_reuse) {
        throw JSONRPCError(RPC_WALLET_ERROR, "wallet does not have the \"avoid reuse\" feature enabled");
    }

    return avoid_reuse;
}


/** Used by RPC commands that have an include_watchonly parameter.
 *  We default to true for watchonly wallets if include_watchonly isn't
 *  explicitly set.
 */
static bool ParseIncludeWatchonly(const UniValue& include_watchonly, const CWallet& pwallet)
{
    if (include_watchonly.isNull()) {
        // if include_watchonly isn't explicitly set, then check if we have a watchonly wallet
        return pwallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    }

    // otherwise return whatever include_watchonly was set to
    return include_watchonly.get_bool();
}


/** Checks if a CKey is in the given CWallet compressed or otherwise*/
bool HaveKey(const SigningProvider& wallet, const CKey& key)
{
    CKey key2;
    key2.Set(key.begin(), key.end(), !key.IsCompressed());
    return wallet.HaveKey(key.GetPubKey().GetID()) || wallet.HaveKey(key2.GetPubKey().GetID());
}

bool GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& wallet_name)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        wallet_name = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        return true;
    }
    return false;
}

std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        std::shared_ptr<CWallet> pwallet = GetWallet(wallet_name);
        if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
        return pwallet;
    }

    std::vector<std::shared_ptr<CWallet>> wallets = GetWallets();
    return wallets.size() == 1 || (request.fHelp && wallets.size() > 0) ? wallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(const CWallet* pwallet)
{
    return pwallet && pwallet->IsCrypted()
        ? "\nRequires wallet passphrase to be set with walletpassphrase call."
        : "";
}

bool EnsureWalletIsAvailable(const CWallet* pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (!HasWallets()) {
        throw JSONRPCError(
            RPC_METHOD_NOT_FOUND, "Method not found (wallet method is disabled because no wallet is loaded)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(const CWallet* pwallet)
{
    if (pwallet->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
}

// also_create should only be set to true only when the RPC is expected to add things to a blank wallet and make it no longer blank
LegacyScriptPubKeyMan& EnsureLegacyScriptPubKeyMan(CWallet& wallet, bool also_create)
{
    LegacyScriptPubKeyMan* spk_man = wallet.GetLegacyScriptPubKeyMan();
    if (!spk_man && also_create) {
        spk_man = wallet.GetOrCreateLegacyScriptPubKeyMan();
    }
    if (!spk_man) {
        throw JSONRPCError(RPC_WALLET_ERROR, "This type of wallet does not support this command");
    }
    return *spk_man;
}

static void WalletTxToJSON(interfaces::Chain& chain, interfaces::Chain::Lock& locked_chain, const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.pushKV("confirmations", confirms);
    if (wtx.IsCoinBase())
        entry.pushKV("generated", true);
    if (confirms > 0)
    {
        entry.pushKV("blockhash", wtx.m_confirm.hashBlock.GetHex());
        entry.pushKV("blockheight", wtx.m_confirm.block_height);
        entry.pushKV("blockindex", wtx.m_confirm.nIndex);
        int64_t block_time;
        bool found_block = chain.findBlock(wtx.m_confirm.hashBlock, nullptr /* block */, &block_time);
        CHECK_NONFATAL(found_block);
        entry.pushKV("blocktime", block_time);
    } else {
        entry.pushKV("trusted", wtx.IsTrusted(locked_chain));
    }
    uint256 hash = wtx.GetHash();
    entry.pushKV("txid", hash.GetHex());
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.pushKV("walletconflicts", conflicts);
    entry.pushKV("time", wtx.GetTxTime());
    entry.pushKV("timereceived", (int64_t)wtx.nTimeReceived);

    // Add opt-in RBF status
    std::string rbfStatus = "no";
    if (confirms <= 0) {
        RBFTransactionState rbfState = chain.isRBFOptIn(*wtx.tx);
        if (rbfState == RBFTransactionState::UNKNOWN)
            rbfStatus = "unknown";
        else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125)
            rbfStatus = "yes";
    }
    entry.pushKV("bip125-replaceable", rbfStatus);

    for (const std::pair<const std::string, std::string>& item : wtx.mapValue)
        entry.pushKV(item.first, item.second);
}

static std::string LabelFromValue(const UniValue& value)
{
    std::string label = value.get_str();
    if (label == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

static UniValue getnewaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getnewaddress",
                "\nReturns a new Bitcoin address for receiving payments.\n"
                "If 'label' is specified, it is added to the address book \n"
                "so payments received with the address will be associated with 'label'.\n",
                {
                    {"label", RPCArg::Type::STR, /* default */ "\"\"", "The label name for the address to be linked to. It can also be set to the empty string \"\" to represent the default label. The label does not need to exist, it will be created if there is no label by the given name."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
            "\"address\"    (string) The new bitcoin address\n"
                },
                RPCExamples{
                    HelpExampleCli("getnewaddress", "")
            + HelpExampleRpc("getnewaddress", "")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    // Parse the label first so we don't generate a key if there's an error
    std::string label;
    if (!request.params[0].isNull())
        label = LabelFromValue(request.params[0]);

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[1].isNull()) {
        if (!ParseOutputType(request.params[1].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[1].get_str()));
        }
    }

    CTxDestination dest;
    std::string error;
    if (!pwallet->GetNewDestination(output_type, label, dest, error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
    }

    return EncodeDestination(dest);
}

static UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getrawchangeaddress",
                "\nReturns a new Bitcoin address, for receiving change.\n"
                "This is for use with raw transactions, NOT normal use.\n",
                {
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
            "\"address\"    (string) The address\n"
                },
                RPCExamples{
                    HelpExampleCli("getrawchangeaddress", "")
            + HelpExampleRpc("getrawchangeaddress", "")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses(true)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    OutputType output_type = pwallet->m_default_change_type != OutputType::CHANGE_AUTO ? pwallet->m_default_change_type : pwallet->m_default_address_type;
    if (!request.params[0].isNull()) {
        if (!ParseOutputType(request.params[0].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[0].get_str()));
        }
    }

    CTxDestination dest;
    std::string error;
    if (!pwallet->GetNewChangeDestination(output_type, dest, error)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
    }
    return EncodeDestination(dest);
}


static UniValue setlabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"setlabel",
                "\nSets the label associated with the given address.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The bitcoin address to be associated with a label."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label to assign to the address."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("setlabel", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"tabby\"")
            + HelpExampleRpc("setlabel", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"tabby\"")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");
    }

    std::string label = LabelFromValue(request.params[1]);

    if (pwallet->IsMine(dest)) {
        pwallet->SetAddressBook(dest, label, "receive");
    } else {
        pwallet->SetAddressBook(dest, label, "send");
    }

    return NullUniValue;
}


static CTransactionRef SendMoney(interfaces::Chain::Lock& locked_chain, CWallet * const pwallet, const CTxDestination &address, CAmount nValue, bool fSubtractFeeFromAmount, const CCoinControl& coin_control, mapValue_t mapValue)
{
    CAmount curBalance = pwallet->GetBalance(0, coin_control.m_avoid_address_reuse).m_mine_trusted;

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);
    CTransactionRef tx;
    if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, nFeeRequired, nChangePosRet, strError, coin_control)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */);
    return tx;
}

static UniValue sendtoaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"sendtoaddress",
                "\nSend an amount to a given address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The bitcoin address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment used to store what the transaction is for.\n"
            "                             This is not part of the transaction, just kept in your wallet."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment to store the name of the person or organization\n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet."},
                    {"subtractfeefromamount", RPCArg::Type::BOOL, /* default */ "false", "The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less bitcoins than you enter in the amount field."},
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\""},
                    {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "true", "(only available if avoid_reuse wallet flag is set) Avoid spending from dirty addresses; addresses are considered\n"
            "                             dirty if they have previously been used in a transaction."},
                },
                RPCResult{
            "\"txid\"                  (string) The transaction id.\n"
                },
                RPCExamples{
                    HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.1, \"donation\", \"seans outpost\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6], pwallet->chain().estimateMaxBlocks());
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    coin_control.m_avoid_address_reuse = GetAvoidReuseFlag(pwallet, request.params[8]);
    // We also enable partial spend avoidance if reuse avoidance is set.
    coin_control.m_avoid_partial_spends |= coin_control.m_avoid_address_reuse;

    EnsureWalletIsUnlocked(pwallet);

    CTransactionRef tx = SendMoney(*locked_chain, pwallet, dest, nAmount, fSubtractFeeFromAmount, coin_control, std::move(mapValue));
    return tx->GetHash().GetHex();
}

static UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listaddressgroupings",
                "\nLists groups of addresses which have had their common ownership\n"
                "made public by common use as inputs or as the resulting change\n"
                "in past transactions\n",
                {},
                RPCResult{
            "[\n"
            "  [\n"
            "    [\n"
            "      \"address\",            (string) The bitcoin address\n"
            "      amount,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"label\"               (string, optional) The label\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listaddressgroupings", "")
            + HelpExampleRpc("listaddressgroupings", "")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);
    for (const std::set<CTxDestination>& grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (const CTxDestination& address : grouping)
        {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwallet->mapAddressBook.find(address) != pwallet->mapAddressBook.end()) {
                    addressInfo.push_back(pwallet->mapAddressBook.find(address)->second.name);
                }
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

static UniValue signmessage(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"signmessage",
                "\nSign a message with the private key of an address" +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The bitcoin address to use for the private key."},
                    {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message to create a signature of."},
                },
                RPCResult{
            "\"signature\"          (string) The signature of the message encoded in base 64\n"
                },
                RPCExamples{
            "\nUnlock the wallet for 30 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"my message\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const PKHash *pkhash = boost::get<PKHash>(&dest);
    if (!pkhash) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CScript script_pub_key = GetScriptForDestination(*pkhash);
    std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(script_pub_key);
    if (!provider) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CKey key;
    CKeyID keyID(*pkhash);
    if (!provider->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(vchSig.data(), vchSig.size());
}

static UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getreceivedbyaddress",
                "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The bitcoin address for transactions."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
            "amount   (numeric) The total amount in " + CURRENCY_UNIT + " received at this address.\n"
                },
                RPCExamples{
            "\nThe amount from transactions with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Bitcoin address
    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");
    }
    CScript scriptPubKey = GetScriptForDestination(dest);
    if (!pwallet->IsMine(scriptPubKey)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");
    }

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !locked_chain->checkFinalTx(*wtx.tx)) {
            continue;
        }

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


static UniValue getreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getreceivedbylabel",
                "\nReturns the total amount received by addresses with <label> in transactions with at least [minconf] confirmations.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The selected label, may be the default label using \"\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this label.\n"
                },
                RPCExamples{
            "\nAmount received by the default label with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbylabel", "\"\"") +
            "\nAmount received at the tabby label including unconfirmed amounts with zero confirmations\n"
            + HelpExampleCli("getreceivedbylabel", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbylabel", "\"tabby\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getreceivedbylabel", "\"tabby\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Get the set of pub keys assigned to label
    std::string label = LabelFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetLabelAddresses(label);

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !locked_chain->checkFinalTx(*wtx.tx)) {
            continue;
        }

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && pwallet->IsMine(address) && setAddress.count(address)) {
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
            }
        }
    }

    return ValueFromAmount(nAmount);
}


static UniValue getbalance(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getbalance",
                "\nReturns the total available balance.\n"
                "The available balance is what the wallet considers currently spendable, and is\n"
                "thus affected by options which limit spendability such as -spendzeroconfchange.\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Remains for backward compatibility. Must be excluded or set to \"*\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "0", "Only include transactions confirmed at least this many times."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also include balance in watch-only addresses (see 'importaddress')"},
                    {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "true", "(only available if avoid_reuse wallet flag is set) Do not include balance in dirty outputs; addresses are considered dirty if they have previously been used in a transaction."},
                },
                RPCResult{
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this wallet.\n"
                },
                RPCExamples{
            "\nThe total amount in the wallet with 1 or more confirmations\n"
            + HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet at least 6 blocks confirmed\n"
            + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("getbalance", "\"*\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    const UniValue& dummy_value = request.params[0];
    if (!dummy_value.isNull() && dummy_value.get_str() != "*") {
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "dummy first argument must be excluded or set to \"*\".");
    }

    int min_depth = 0;
    if (!request.params[1].isNull()) {
        min_depth = request.params[1].get_int();
    }

    bool include_watchonly = ParseIncludeWatchonly(request.params[2], *pwallet);

    bool avoid_reuse = GetAvoidReuseFlag(pwallet, request.params[3]);

    const auto bal = pwallet->GetBalance(min_depth, avoid_reuse);

    return ValueFromAmount(bal.m_mine_trusted + (include_watchonly ? bal.m_watchonly_trusted : 0));
}

static UniValue getunconfirmedbalance(const JSONRPCRequest &request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getunconfirmedbalance",
                "DEPRECATED\nIdentical to getbalances().mine.untrusted_pending\n",
                {},
                RPCResults{},
                RPCExamples{""},
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetBalance().m_mine_untrusted_pending);
}


static UniValue sendmany(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"sendmany",
                "\nSend multiple times. Amounts are double-precision floating point numbers." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::NO, "Must be set to \"\" for backwards compatibility.", "\"\""},
                    {"amounts", RPCArg::Type::OBJ, RPCArg::Optional::NO, "A json object with addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The bitcoin address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "Ignored dummy value"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {"subtractfeefrom", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array with addresses.\n"
            "                           The fee will be equally deducted from the amount of each selected address.\n"
            "                           Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
            "                           If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Subtract fee from this address"},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\""},
                },
                 RPCResult{
            "\"txid\"                   (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"
                 },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 1 \"\" \"[\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\":0.01,\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, 6, \"testing\"")
                },
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    UniValue sendTo = request.params[1].get_obj();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6], pwallet->chain().estimateMaxBlocks());
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::set<CTxDestination> destinations;
    std::vector<CRecipient> vecSend;

    std::vector<std::string> keys = sendTo.getKeys();
    for (const std::string& name_ : keys) {
        CTxDestination dest = DecodeDestination(name_);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Bitcoin address: ") + name_);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
        }
        destinations.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked(pwallet);

    // Shuffle recipient list
    std::shuffle(vecSend.begin(), vecSend.end(), FastRandomContext());

    // Send
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;
    CTransactionRef tx;
    bool fCreated = pwallet->CreateTransaction(*locked_chain, vecSend, tx, nFeeRequired, nChangePosRet, strFailReason, coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */);
    return tx->GetHash().GetHex();
}

static UniValue sendmanycompacted(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const swallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = swallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }
    CWallet& wallet = *pwallet;

    RPCHelpMan{"sendmanycompacted",
                "\nSend multiple times. Amounts are double-precision floating point numbers." +
                    HelpRequiringPassphrase(&wallet) + "\n",
                {
                    {"amounts", RPCArg::Type::OBJ, RPCArg::Optional::NO, "A json object with addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The bitcoin address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"radix", RPCArg::Type::NUM, /* default */ "4", "Radix to use for the tree"},
                    {"gas", RPCArg::Type::NUM, /* default */ "0", "Adds gas dust outputs if gas > 471 (satoshis)"},
                    {"node_fees", RPCArg::Type::STR, /* default */ "MIN", "The interior node fees. Must be one of:\n"
                    "       \"MIN\"\n"
                    "       \"ECONOMICAL\"\n"
                    "       \"CONSERVATIVE\"\n"
                    },
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {"root_replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"root_conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"root_estimate", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                    "       \"UNSET\"\n"
                    "       \"ECONOMICAL\"\n"
                    "       \"CONSERVATIVE\""},
                    {"pairing_mode", RPCArg::Type::STR, /* default */ "AS_IS", "Re-order the outputs by:\n"
                    "       \"AS_IS\"\n"
                    "       \"LEXICOGRAPHIC\"\n"
                    "       \"PROBABILITY\"\n"
                    "       \"BALANCE_PROBABILITY\"\n"
                    "       \"BALANCE_VALUE\"\n"
                    "       \"VALUE\""},
                    {"probabilities", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of redemption probabilities",
                        {
                            {"probability", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "floating point probability"},
                        },
                    },
                },
                 RPCResult{
            "\"[txids]\"                   (string) The transaction ids for the send.\n"
                 },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmanycompacted", "\"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the radix to 4:\n"
            + HelpExampleCli("sendmanycompacted", "\"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 4 ") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmanycompacted", "{\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\":0.01,\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, 4")
                },
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    auto locked_chain = wallet.chain().lock();
    LOCK(wallet.cs_wallet);
    const size_t AMOUNTS = 0;
    const size_t RADIX = 1;
    const size_t GAS = 2;
    const size_t NODE_FEES = 3;
    const size_t COMMENT = 4;
    const size_t ROOT_REPLACEABLE = 5;
    const size_t ROOT_CONF_TARGET = 6;
    const size_t ROOT_ESTIMATE = 7;
    const size_t PAIRING_MODE = 8;
    const size_t PROBABILITIES = 9;
    UniValue sendTo = request.params[AMOUNTS].get_obj();
    size_t radix = 4;
    bool radix_is_child_limit = false;
    if (!request.params[RADIX].isNull()) {
        int radixIn = request.params[RADIX].get_int();
        if (radixIn == 0) {
            radix = sendTo.getKeys().size();
        } else if (radixIn == 1) {
            radix = 1;
        } else if (radixIn < -1 ) {
            radix =  -radixIn;
            radix_is_child_limit = true;

        } else if (radixIn < 2) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Radix must be 2 or more");
        } else {
            radix = (size_t)radixIn;
        }
    }
    CAmount gas = 0;
    if (!request.params[GAS].isNull()) {
        int64_t gasIn = request.params[GAS].get_int64();
        if (gasIn < 472 && gasIn > 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Gas must be > 471");
        } else if (gasIn < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Gas must be >= 0");
        } if (gasIn > 472*30) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Gas must be <= 14160");
        }
        gas = gasIn;
    }
    std::string node_fees = "MIN";
    if (!request.params[NODE_FEES].isNull()) {
        node_fees = request.params[NODE_FEES].get_str();
        if (node_fees != "MIN" && node_fees != "CONSERVATIVE" && node_fees != "ECONOMICAL") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    mapValue_t mapValue;
    if (!request.params[COMMENT].isNull() && !request.params[COMMENT].get_str().empty())
        mapValue["comment"] = request.params[COMMENT].get_str();

    CCoinControl coin_control;
    if (!request.params[ROOT_REPLACEABLE].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[ROOT_REPLACEABLE].get_bool();
    }

    if (!request.params[ROOT_CONF_TARGET].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[ROOT_CONF_TARGET], wallet.chain().estimateMaxBlocks());
    }

    if (!request.params[ROOT_ESTIMATE].isNull()) {
        if (!FeeModeFromString(request.params[ROOT_ESTIMATE].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::string pairing_mode = "AS_IS";
    if (!request.params[PAIRING_MODE].isNull()) {
        pairing_mode = request.params[PAIRING_MODE].get_str();
    }

    std::vector<std::string> keys = sendTo.getKeys();

    if (radix_is_child_limit) {
        // n/r + n/r**2 + n/r**3.... = limit
        // n/r ( 1+1/r +1/r**2.... ) = limit
        // r = n / limit + 1
        radix = (keys.size()/radix) +1;
        radix_is_child_limit = false;
    }

    std::vector<std::pair<double, size_t>> sort_keys;
    if (pairing_mode == "PROBABILITY" || pairing_mode == "BALANCE_PROBABILITY") {
        if (!request.params[PROBABILITIES].isNull()) {
            UniValue weights = request.params[PAIRING_MODE].get_array();
            for (unsigned int idx = 0; idx < weights.size(); idx++) {
                const double& weight = weights[idx].get_real();
                sort_keys.emplace_back(weight, idx);
            }
        }
    }
    else if (pairing_mode == "VALUE" || pairing_mode == "BALANCE_VALUE"){
        std::vector<UniValue> balances = sendTo.getValues();
        size_t count = 0;
        for (auto& balance : balances) {
            sort_keys.emplace_back(AmountFromValue(balance), count++);
        }
    }
    else if (pairing_mode == "LEXICOGRAPHIC") {
        std::sort(keys.begin(), keys.end());
        sort_keys.resize(keys.size());
        size_t count = 0;
        for (auto& sort_key : sort_keys) {
            sort_key.second = count++;
        }
    }
    else if (pairing_mode == "AS_IS") {
        sort_keys.resize(keys.size());
        size_t count = 0;
        for (auto& sort_key : sort_keys) {
            sort_key.second = count++;
        }
    } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid pairing_mode parameter");
    }
    if (sort_keys.size() != keys.size()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid probabilities parameter");
    }
    const bool balance_mode = pairing_mode.find("BALANCE_") != std::string::npos;


    std::set<CTxDestination> destinations;

    // The multiset keeps the items in vecSend sorted by weight
    const auto cmp = [](const std::pair<double, CTxOut>& a, const std::pair<double, CTxOut>& b) -> bool {
            return a.first < b.first;
            };
    std::multiset<std::pair<double, CTxOut>, decltype(cmp)> vecSend(cmp);
    auto sort_keys_it = sort_keys.begin();
    for (const std::string& name_ : keys) {
        CTxDestination dest = DecodeDestination(name_);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Bitcoin address: ") + name_);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
        }
        destinations.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

        CTxOut recipient = {nAmount, scriptPubKey};
        vecSend.emplace((sort_keys_it++)->first, recipient);
    }

    // N.B. This map relies on the de-duplication above, otherwise you could
    // have identical sub-trees which would assemble the incorrect number of
    // transactions later on (& triggers the assert below)
    std::map<uint256, CMutableTransaction> templates;
    const CFeeRate calc_fee = wallet.chain().relayMinFee();
    std::vector<unsigned char> h_buff(32);
    if (radix == 1) {

        CMutableTransaction base{};
        for (auto& txo : vecSend) {
            CMutableTransaction tx{};
            tx.vout.emplace_back(std::move(txo.second));
            tx.vin.emplace_back();
            if (base.vin.size()) {
                const uint256 hash = GetStandardTemplateHash(base, 0);
                const bool inserted = templates.insert(std::make_pair(hash, std::move(base))).second;
                CHECK_NONFATAL(inserted); // is guaranteed to be unique
                std::memmove(h_buff.data(), hash.begin(), 32);
                tx.vout.emplace_back(0, CScript() << h_buff << OP_CHECKTEMPLATEVERIFY);
                CAmount amount = calc_fee.GetFee(GetSerializeSize(tx));
                // TODO: compute fee from args?
                for (const auto& out : tx.vout) amount += out.nValue;
                tx.vout[0].nValue = amount;
            }
            base = std::move(tx);
        }
        vecSend.clear();

        const uint256 hash = GetStandardTemplateHash(base, 0);
        const bool inserted = templates.insert(std::make_pair(hash, base)).second;
        CHECK_NONFATAL(inserted); // is guaranteed to be unique

        std::memmove(h_buff.data(), hash.begin(), 32);
        CScript script = CScript() << h_buff << OP_CHECKTEMPLATEVERIFY;
        vecSend.emplace(0, CTxOut(base.vout[0].nValue + base.vout[1].nValue, script));

    } else {
        std::vector<std::pair<double, CTxOut>> items;
        items.reserve(radix);
        while (vecSend.size() > 1) {
            CMutableTransaction tx;
            // Add one input for proper Bag Hash computation
            // and fee computation
            tx.vin.emplace_back();
            items.clear();
            const size_t pieces = std::min(radix, vecSend.size());
            if (balance_mode) {
                // TODO: better greedy algorithm?
                // take half from the back, half from the front of the set.
                // the front are small, the back is large
                size_t big_pieces = pieces/2;
                // take more small peices than big pieces
                size_t small_pieces = pieces - big_pieces;
                if (small_pieces) {
                    auto start = vecSend.begin();
                    auto end = vecSend.begin();
                    std::advance(end, small_pieces);
                    items.insert(items.begin(), std::make_move_iterator(start), std::make_move_iterator(end));
                    vecSend.erase(start, end);
                }
                if (big_pieces) {
                    auto start = vecSend.rbegin();
                    auto end = vecSend.rbegin();
                    std::advance(end, big_pieces);
                    items.insert(items.end(), std::make_move_iterator(start), std::make_move_iterator(end));
                    vecSend.erase(std::prev(start.base()), vecSend.end());
                }
            } else { // unbalanced mode!
                auto start = vecSend.begin();
                auto end = vecSend.begin();
                std::advance(end, pieces);
                items.insert(items.end(), std::make_move_iterator(start), std::make_move_iterator(end));
                vecSend.erase(start, end);
            }
            // Sum the weight for the new send by the children's weight
            double new_weight = 0;
            for (auto& item: items) {
                new_weight += item.first;
                tx.vout.push_back(item.second);
            }
            if (gas) {
                tx.vout.emplace_back(gas, CScript() << OP_TRUE);
            }
            const uint256 hash = GetStandardTemplateHash(tx, 0);
            std::memmove(h_buff.data(), hash.begin(), 32);
            // TODO: compute fee from args?
            CAmount amount = calc_fee.GetFee(GetSerializeSize(tx));
            for (const auto& out : tx.vout) amount += out.nValue;
            const bool inserted = templates.emplace(hash, std::move(tx)).second;
            CHECK_NONFATAL(inserted); // is guaranteed to be unique
            vecSend.emplace(new_weight, CTxOut(amount, CScript() << h_buff << OP_CHECKTEMPLATEVERIFY));
        }
    }
    EnsureWalletIsUnlocked(&wallet);

    // Create and Commit the root transaction to the wallet.
    CTransactionRef tx;
    int root_idx = -1;
    std::string fail_reason;
    {
        std::vector<CRecipient> rec {{vecSend.begin()->second.scriptPubKey, vecSend.begin()->second.nValue, false}};
        // Send
        CAmount fee_required = 0;
        if (!wallet.CreateTransaction(*locked_chain, rec, tx, fee_required, root_idx, fail_reason, coin_control))
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, fail_reason);
        // mod tx.vout.size() in case there is no change.
        // expected to be 2, may be 1
        root_idx = (root_idx +1)%tx->vout.size();
    }

    // Fill out the templates for all the children transactions and Commit
    std::vector<std::pair<COutPoint, uint256>> stack {{{tx->GetHash(), (uint32_t)root_idx}, {}}};
    std::memmove(stack.back().second.begin(), tx->vout[root_idx].scriptPubKey.data()+1, 32);



    UniValue txns(UniValue::VARR);
    UniValue txn(UniValue::VOBJ);
    txn.pushKV("hex", EncodeHexTx(*tx, wallet.chain().rpcSerializationFlags()));
    txn.pushKV("label", "create_tx");
    txn.pushKV("color", "yellow");
    txns.push_back(txn);
    while (!stack.empty()) {
        CMutableTransaction  mtx = std::move(templates[stack.back().second]);
        mtx.vin[0].prevout = stack.back().first;
        stack.pop_back();
        CTransactionRef tx = MakeTransactionRef(std::move(mtx));
        const uint256 txid = tx->GetHash();
        UniValue txn(UniValue::VOBJ);
        txn.pushKV("hex", EncodeHexTx(*tx, wallet.chain().rpcSerializationFlags()));
        txn.pushKV("label", "tree");
        txn.pushKV("color", "brown");
        UniValue utxo_metadata(UniValue::VARR);
        for (const auto& out : tx->vout) {
            UniValue meta(UniValue::VOBJ);
            if (out.scriptPubKey.IsPayToBasicStandardTemplate()) {
                meta.pushKV("label","interior node");
                meta.pushKV("color","grey");
            } else if (out.nValue == gas && out.scriptPubKey.size() == 1 && out.scriptPubKey[0] == OP_TRUE) {
                meta.pushKV("label", "gas");
                meta.pushKV("color","red");
            } else {
                meta.pushKV("label","payment");
                meta.pushKV("color","green");
            }
            utxo_metadata.push_back(meta);
        }
        txn.pushKV("utxo_metadata", utxo_metadata);
        txns.push_back(txn);


        uint32_t i = 0;
        for (auto& out : tx->vout) {
            if (out.scriptPubKey.IsPayToBasicStandardTemplate()) {
                stack.emplace_back(COutPoint{txid, i}, uint256{});
                std::memmove(stack.back().second.begin(), out.scriptPubKey.data()+1, 32);
            }
            ++i;
        }
    }

    UniValue metadata(UniValue::VOBJ);
    UniValue outpoint(UniValue::VOBJ);
    outpoint.pushKV("hash", tx->GetHash().ToString());
    outpoint.pushKV("n", 0);
    metadata.pushKV("radix", int(radix));

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("metadata", metadata);
    ret.pushKV("program", txns);
    return ret;
}



static UniValue create_ctv_vault(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const swallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = swallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }
    CWallet& wallet = *pwallet;

            RPCHelpMan{"create_ctv_vault",
                "\nSend an amount to a given address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount per step in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"steps", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of steps to take."},
                    {"step_period", RPCArg::Type::NUM, RPCArg::Optional::NO, "The amount per step in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"maturity", RPCArg::Type::NUM, RPCArg::Optional::NO, "The amount per step in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"hot_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "The bitcoin address to send to for hot storage."},
                    {"cold_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "The bitcoin address to send to for cold storage."},
                },
                RPCResult{
            "\"txid\"                  (string) The transaction id.\n"
                },
                RPCExamples{
                    HelpExampleCli("create_ctv_vault", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("create_ctv_vault", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("create_ctv_vault", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("create_ctv_vault", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.1, \"donation\", \"seans outpost\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    auto locked_chain = wallet.chain().lock();
    LOCK(wallet.cs_wallet);
    const size_t AMOUNT = 0;
    const size_t STEPS = 1;
    const size_t STEP_PERIOD = 2;
    const size_t MATURITY = 3;
    const size_t HOT_ADDRESS = 4;
    const size_t COLD_ADDRESS = 5;
    const CAmount amount = AmountFromValue(request.params[AMOUNT]);
    const int64_t steps = request.params[STEPS].get_int64();
    if (steps <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Must have at least 1 step.");
    const CAmount total_amount = steps*amount;
    if ((total_amount / steps != amount) || !MoneyRange(total_amount)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Total money too much.");

    const auto bal = wallet.GetBalance(1, false);
    if (bal.m_mine_trusted < total_amount) throw JSONRPCError(RPC_INVALID_PARAMETER, "Insufficient funds.");

    const int64_t step_period = request.params[STEP_PERIOD].get_int64();
    if (step_period < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "step_period must be positive number of blocks.");
    if (step_period > CTxIn::SEQUENCE_LOCKTIME_MASK)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "step_period must be less than SEQUENCE_LOCKTIME_MASK");
    const int64_t maturity = request.params[MATURITY].get_int64();
    if (maturity < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Maturity must be positive number of blocks.");
    if (maturity > CTxIn::SEQUENCE_LOCKTIME_MASK)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "maturity must be less than SEQUENCE_LOCKTIME_MASK");

    auto get_new_dest = [&]()->CTxDestination{
        ///
        if (!wallet.CanGetAddresses()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
        }
        CTxDestination dest;
        std::string error;
        if (!wallet.GetNewDestination(wallet.m_default_address_type, "", dest, error)) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, error);
        }
        return dest;
    };
    CTxDestination hot_dest = request.params[HOT_ADDRESS].isNull() ? get_new_dest() : DecodeDestination(request.params[HOT_ADDRESS].get_str());
    if (!IsValidDestination(hot_dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    CTxDestination cold_dest = request.params[COLD_ADDRESS].isNull() ? get_new_dest() : DecodeDestination(request.params[COLD_ADDRESS].get_str());
    if (!IsValidDestination(cold_dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    struct VaultTemplate {
        CMutableTransaction vault_to_vault, hot_to_hot, vault_to_cold, hot_to_cold;
        CAmount amount = 0;
        /*
         *                   [      vault_to_vault n  ]
         *                     |a                   |b
         *                    (0)                  (1)
         *                    /\                   /\
         * wait + maturity   /  \                 /  \ wait + step_period    (attached)
         * [   hot_to_hot   ]   /  [vault_to_cold]    [vault_to_vault n - 1]
         *                     /                         |           |
         *        [hot_to_cold]                         (0)         (1)
         *                                               .           .
         *                                               .           .
         *                                                           .
         *                                                     wait + step_period * n   (attached)
         *                                                   [vault_to_vault 0]
         *                                                      |
         *                                                     (0)
         *                                                     */

        VaultTemplate() {}

        VaultTemplate(CAmount a, CAmount b, CAmount f, uint32_t hot_pause, uint32_t vault_pause,
                CTxDestination& hot, CTxDestination& cold, VaultTemplate* vault_to_attach) {
            hot_to_cold.vin.resize(1);
            hot_to_cold.vout.resize(1);
            hot_to_cold.vout[0].nValue = a;
            hot_to_cold.vout[0].scriptPubKey = GetScriptForDestination(cold);
            hot_to_cold.vin[0].prevout.n = 0;

            hot_to_hot.vin.resize(1);
            hot_to_hot.vout.resize(1);
            hot_to_hot.vout[0].nValue = a;
            hot_to_hot.vout[0].scriptPubKey = GetScriptForDestination(hot);
            hot_to_hot.vin[0].nSequence = hot_pause;
            hot_to_hot.vin[0].prevout.n = 0;

            vault_to_cold.vin.resize(1);
            vault_to_cold.vout.resize(1);
            vault_to_cold.vout[0].nValue = b;
            vault_to_cold.vout[0].scriptPubKey = GetScriptForDestination(cold);
            vault_to_cold.vin[0].prevout.n = 1;

            vault_to_vault.vin.resize(1);
            vault_to_vault.vin[0].nSequence = vault_pause;
            vault_to_vault.vin[0].prevout.n = 1;
            vault_to_vault.vout.emplace_back();

            std::vector<unsigned char> h_buff(32);
            {
                CScript vault_to_vault_redeem{};
                vault_to_vault.vout[0].nValue = a+f;
                amount += f + a;
                // construct script
                uint256 hash = GetStandardTemplateHash(hot_to_cold, 0);
                std::memmove(h_buff.data(), hash.begin(), 32);
                vault_to_vault_redeem << OP_IF << h_buff << OP_ELSE;
                hash = GetStandardTemplateHash(hot_to_hot, 0);
                std::memmove(h_buff.data(), hash.begin(), 32);
                vault_to_vault_redeem << h_buff << OP_ENDIF << OP_CHECKTEMPLATEVERIFY;
                vault_to_vault.vout[0].scriptPubKey = GetScriptForWitness(vault_to_vault_redeem);
                hot_to_cold.vin[0].scriptWitness.stack.push_back({1});
                hot_to_cold.vin[0].scriptWitness.stack.push_back({vault_to_vault_redeem.begin(), vault_to_vault_redeem.end()});
                hot_to_hot.vin[0].scriptWitness.stack.push_back({});
                hot_to_hot.vin[0].scriptWitness.stack.push_back({vault_to_vault_redeem.begin(), vault_to_vault_redeem.end()});

            }
            if (b != 0 && vault_to_attach!=nullptr){
                CScript vault_to_vault_redeem{};
                vault_to_vault.vout.emplace_back();
                vault_to_vault.vout[1].nValue = b+f;
                amount += b + f;
                uint256 hash = GetStandardTemplateHash(vault_to_cold, 0);
                std::memmove(h_buff.data(), hash.begin(), 32);
                vault_to_vault_redeem << OP_IF << h_buff << OP_ELSE;
                hash = GetStandardTemplateHash(vault_to_attach->vault_to_vault, 0);
                std::memmove(h_buff.data(), hash.begin(), 32);
                vault_to_vault_redeem << h_buff << OP_ENDIF << OP_CHECKTEMPLATEVERIFY;
                vault_to_vault.vout[1].scriptPubKey = GetScriptForWitness(vault_to_vault_redeem);
                vault_to_cold.vin[0].scriptWitness.stack.push_back({1});
                vault_to_cold.vin[0].scriptWitness.stack.push_back({vault_to_vault_redeem.begin(), vault_to_vault_redeem.end()});
                vault_to_attach->vault_to_vault.vin[0].scriptWitness.stack.push_back({});
                vault_to_attach->vault_to_vault.vin[0].scriptWitness.stack.push_back({vault_to_vault_redeem.begin(), vault_to_vault_redeem.end()});

            }

        }
        void attach(const uint256& hashIn, Optional<uint32_t> n) {
            vault_to_vault.vin[0].prevout.hash = hashIn;
            if (n) vault_to_vault.vin[0].prevout.n = *n;
            uint256 hash = vault_to_vault.GetHash();
            hot_to_hot.vin[0].prevout.hash = hash;
            hot_to_cold.vin[0].prevout.hash = hash;
            vault_to_cold.vin[0].prevout.hash = hash;

        }
        void attach(VaultTemplate& sub_template) {
            uint256 hash = sub_template.vault_to_vault.GetHash();
            attach(hash, nullopt);
        }
    };
    // TODO: Dynamic Fees.
    // TODO: Higher feerate on move to cold-storage
    const CAmount flat_fee = 1000;
    std::vector<VaultTemplate> templates(steps);
    templates.back() = VaultTemplate(amount, 0, flat_fee, maturity, step_period, hot_dest, cold_dest, nullptr);
    // steps -2, steps -3... 0
    for (uint64_t i = (steps - 1) ; i-- > 0;) {
        templates[i] = VaultTemplate(amount, templates[i+1].amount, flat_fee, maturity, step_period, hot_dest, cold_dest, &templates[i +1]);
    }
    EnsureWalletIsUnlocked(&wallet);

    CTransactionRef tx;
    std::string fail_reason;
    {
        std::vector<unsigned char> h_buff(32);
        uint256 hash = GetStandardTemplateHash(templates[0].vault_to_vault, 0);
        std::memmove(h_buff.data(), hash.begin(), 32);
        std::vector<CRecipient> rec {{CScript() << h_buff << OP_CHECKTEMPLATEVERIFY, templates[0].amount + flat_fee, false}};
        // Send
        CAmount fee_required = 0;
        CCoinControl coin_control;
        int change_at = 0;
        if (!wallet.CreateTransaction(*locked_chain, rec, tx, fee_required, change_at, fail_reason, coin_control))
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, fail_reason);
        // mod tx.vout.size() in case there is no change.
        templates[0].attach(tx->GetHash(), Optional<uint32_t>(1 % tx->vout.size()));
    }

    for (auto i = 1; i < steps; ++i) {
        templates[i].attach(templates[i-1]);
    }


    // Fill out the templates for all the children transactions and Commit
    std::vector<UniValue> txns;

    UniValue parent(UniValue::VOBJ);
    parent.pushKV("hex", EncodeHexTx(*tx, wallet.chain().rpcSerializationFlags()));
    parent.pushKV("label", "create_tx");
    parent.pushKV("color", "grey");
    txns.push_back(parent);
    for (auto it = templates.begin(); it != templates.end();) {
        auto& tmpl = *it;
        UniValue vault_to_vault(UniValue::VOBJ);
        vault_to_vault.pushKV("hex", EncodeHexTx(CTransaction(tmpl.vault_to_vault), wallet.chain().rpcSerializationFlags()));
        vault_to_vault.pushKV("label", "vault_to_vault");
        vault_to_vault.pushKV("color", "green");
        UniValue vault_to_cold(UniValue::VOBJ);
        vault_to_cold.pushKV("hex", EncodeHexTx(CTransaction(tmpl.vault_to_cold), wallet.chain().rpcSerializationFlags()));
        vault_to_cold.pushKV("label", "vault_to_cold");
        vault_to_cold.pushKV("color", "blue");
        UniValue to_hot(UniValue::VOBJ);
        to_hot.pushKV("hex", EncodeHexTx(CTransaction(tmpl.hot_to_hot), wallet.chain().rpcSerializationFlags()));
        to_hot.pushKV("label", "to_hot");
        to_hot.pushKV("color", "red");
        UniValue to_cold(UniValue::VOBJ);
        to_cold.pushKV("hex", EncodeHexTx(CTransaction(tmpl.hot_to_cold), wallet.chain().rpcSerializationFlags()));
        to_cold.pushKV("label", "to_cold");
        to_cold.pushKV("color", "blue");

        txns.push_back(vault_to_vault);
        txns.push_back(to_hot);
        txns.push_back(to_cold);
        if (++it != templates.end()) {
            txns.push_back(vault_to_cold);
        }

    }
    UniValue metadata(UniValue::VOBJ);
    UniValue outpoint(UniValue::VOBJ);
    outpoint.pushKV("hash", tx->GetHash().ToString());
    outpoint.pushKV("n", 1);
    metadata.pushKV("prevout", outpoint);
    metadata.pushKV("steps", steps);
    metadata.pushKV("amount", amount);
    metadata.pushKV("total_amount", templates[0].amount);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("metadata", metadata);
    UniValue arr(UniValue::VARR);
    arr.push_backV(txns);
    ret.pushKV("program", arr);
    return ret;
}

static UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"addmultisigaddress",
                "\nAdd an nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
                "Each key is a Bitcoin address or hex-encoded public key.\n"
                "This functionality is only intended for use with non-watchonly addresses.\n"
                "See `importaddress` for watchonly p2sh address support.\n"
                "If 'label' is specified, assign address to that label.\n",
                {
                    {"nrequired", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of required signatures out of the n keys or addresses."},
                    {"keys", RPCArg::Type::ARR, RPCArg::Optional::NO, "A json array of bitcoin addresses or hex-encoded public keys",
                        {
                            {"key", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "bitcoin address or hex-encoded public key"},
                        },
                        },
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A label to assign the addresses to."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
            "{\n"
            "  \"address\":\"multisigaddress\",    (string) The value of the new multisig address.\n"
            "  \"redeemScript\":\"script\"         (string) The string value of the hex-encoded redemption script.\n"
            "}\n"
                },
                RPCExamples{
            "\nAdd a multisig address from 2 addresses\n"
            + HelpExampleCli("addmultisigaddress", "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"")
                },
            }.Check(request);

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet);

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, spk_man.cs_KeyStore);

    std::string label;
    if (!request.params[2].isNull())
        label = LabelFromValue(request.params[2]);

    int required = request.params[0].get_int();

    // Get the public keys
    const UniValue& keys_or_addrs = request.params[1].get_array();
    std::vector<CPubKey> pubkeys;
    for (unsigned int i = 0; i < keys_or_addrs.size(); ++i) {
        if (IsHex(keys_or_addrs[i].get_str()) && (keys_or_addrs[i].get_str().length() == 66 || keys_or_addrs[i].get_str().length() == 130)) {
            pubkeys.push_back(HexToPubKey(keys_or_addrs[i].get_str()));
        } else {
            pubkeys.push_back(AddrToPubKey(spk_man, keys_or_addrs[i].get_str()));
        }
    }

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[3].isNull()) {
        if (!ParseOutputType(request.params[3].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[3].get_str()));
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner;
    CTxDestination dest = AddAndGetMultisigDestination(required, pubkeys, output_type, spk_man, inner);
    pwallet->SetAddressBook(dest, label, "send");

    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(dest));
    result.pushKV("redeemScript", HexStr(inner.begin(), inner.end()));
    return result;
}

struct tallyitem
{
    CAmount nAmount{0};
    int nConf{std::numeric_limits<int>::max()};
    std::vector<uint256> txids;
    bool fIsWatchonly{false};
    tallyitem()
    {
    }
};

static UniValue ListReceived(interfaces::Chain::Lock& locked_chain, CWallet * const pwallet, const UniValue& params, bool by_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (!params[0].isNull())
        nMinDepth = params[0].get_int();

    // Whether to include empty labels
    bool fIncludeEmpty = false;
    if (!params[1].isNull())
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(params[2], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool has_filtered_address = false;
    CTxDestination filtered_address = CNoDestination();
    if (!by_label && params.size() > 3) {
        if (!IsValidDestinationString(params[3].get_str())) {
            throw JSONRPCError(RPC_WALLET_ERROR, "address_filter parameter was invalid");
        }
        filtered_address = DecodeDestination(params[3].get_str());
        has_filtered_address = true;
    }

    // Tally
    std::map<CTxDestination, tallyitem> mapTally;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;

        if (wtx.IsCoinBase() || !locked_chain.checkFinalTx(*wtx.tx)) {
            continue;
        }

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            if (has_filtered_address && !(filtered_address == address)) {
                continue;
            }

            isminefilter mine = pwallet->IsMine(address);
            if(!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> label_tally;

    // Create mapAddressBook iterator
    // If we aren't filtering, go from begin() to end()
    auto start = pwallet->mapAddressBook.begin();
    auto end = pwallet->mapAddressBook.end();
    // If we are filtering, find() the applicable entry
    if (has_filtered_address) {
        start = pwallet->mapAddressBook.find(filtered_address);
        if (start != end) {
            end = std::next(start);
        }
    }

    for (auto item_it = start; item_it != end; ++item_it)
    {
        const CTxDestination& address = item_it->first;
        const std::string& label = item_it->second.name;
        auto it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (by_label)
        {
            tallyitem& _item = label_tally[label];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        }
        else
        {
            UniValue obj(UniValue::VOBJ);
            if(fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("address",       EncodeDestination(address));
            obj.pushKV("amount",        ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label", label);
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end())
            {
                for (const uint256& _item : (*it).second.txids)
                {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.pushKV("txids", transactions);
            ret.push_back(obj);
        }
    }

    if (by_label)
    {
        for (const auto& entry : label_tally)
        {
            CAmount nAmount = entry.second.nAmount;
            int nConf = entry.second.nConf;
            UniValue obj(UniValue::VOBJ);
            if (entry.second.fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("amount",        ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label",         entry.first);
            ret.push_back(obj);
        }
    }

    return ret;
}

static UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listreceivedbyaddress",
                "\nList balances by receiving address.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include addresses that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses (see 'importaddress')"},
                    {"address_filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If present, only return information on this address."},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,        (bool) Only returns true if imported addresses were involved in transaction.\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in " + CURRENCY_UNIT + " received by the address\n"
            "    \"confirmations\" : n,               (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\",               (string) The label of the receiving address. The default label is \"\".\n"
            "    \"txids\": [\n"
            "       \"txid\",                         (string) The ids of transactions received with the address \n"
            "       ...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listreceivedbyaddress", "")
            + HelpExampleCli("listreceivedbyaddress", "6 true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true, \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ListReceived(*locked_chain, pwallet, request.params, false);
}

static UniValue listreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listreceivedbylabel",
                "\nList received transactions by label.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include labels that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Whether to include watch-only addresses (see 'importaddress')"},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,   (bool) Only returns true if imported addresses were involved in transaction.\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this label\n"
            "    \"confirmations\" : n,          (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\"           (string) The label of the receiving address. The default label is \"\".\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listreceivedbylabel", "")
            + HelpExampleCli("listreceivedbylabel", "6 true")
            + HelpExampleRpc("listreceivedbylabel", "6, true, true")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ListReceived(*locked_chain, pwallet, request.params, true);
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    if (IsValidDestination(dest)) {
        entry.pushKV("address", EncodeDestination(dest));
    }
}

/**
 * List transactions based on the given criteria.
 *
 * @param  pwallet        The wallet.
 * @param  wtx            The wallet transaction.
 * @param  nMinDepth      The minimum confirmation depth.
 * @param  fLong          Whether to include the JSON version of the transaction.
 * @param  ret            The UniValue into which the result is stored.
 * @param  filter_ismine  The "is mine" filter flags.
 * @param  filter_label   Optional label string to filter incoming transactions.
 */
static void ListTransactions(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const CWalletTx& wtx, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter_ismine, const std::string* filter_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    CAmount nFee;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, filter_ismine);

    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if (!filter_label)
    {
        for (const COutputEntry& s : listSent)
        {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (pwallet->IsMine(s.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, s.destination);
            entry.pushKV("category", "send");
            entry.pushKV("amount", ValueFromAmount(-s.amount));
            if (pwallet->mapAddressBook.count(s.destination)) {
                entry.pushKV("label", pwallet->mapAddressBook[s.destination].name);
            }
            entry.pushKV("vout", s.vout);
            entry.pushKV("fee", ValueFromAmount(-nFee));
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            entry.pushKV("abandoned", wtx.isAbandoned());
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
        for (const COutputEntry& r : listReceived)
        {
            std::string label;
            if (pwallet->mapAddressBook.count(r.destination)) {
                label = pwallet->mapAddressBook[r.destination].name;
            }
            if (filter_label && label != *filter_label) {
                continue;
            }
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (pwallet->IsMine(r.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, r.destination);
            if (wtx.IsCoinBase())
            {
                if (wtx.GetDepthInMainChain() < 1)
                    entry.pushKV("category", "orphan");
                else if (wtx.IsImmatureCoinBase())
                    entry.pushKV("category", "immature");
                else
                    entry.pushKV("category", "generate");
            }
            else
            {
                entry.pushKV("category", "receive");
            }
            entry.pushKV("amount", ValueFromAmount(r.amount));
            if (pwallet->mapAddressBook.count(r.destination)) {
                entry.pushKV("label", label);
            }
            entry.pushKV("vout", r.vout);
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            ret.push_back(entry);
        }
    }
}

static const std::string TransactionDescriptionString()
{
    return "    \"confirmations\": n,                        (numeric) The number of confirmations for the transaction. Negative confirmations means the\n"
           "                                                       transaction conflicted that many blocks ago.\n"
           "    \"generated\": xxx,                          (bool) Only present if transaction only input is a coinbase one.\n"
           "    \"trusted\": xxx,                            (bool) Only present if we consider transaction to be trusted and so safe to spend from.\n"
           "    \"blockhash\": \"hashvalue\",                  (string) The block hash containing the transaction.\n"
           "    \"blockheight\": n,                          (numeric) The block height containing the transaction.\n"
           "    \"blockindex\": n,                           (numeric) The index of the transaction in the block that includes it.\n"
           "    \"blocktime\": xxx,                          (numeric) The block time expressed in " + UNIX_EPOCH_TIME + ".\n"
           "    \"txid\": \"transactionid\",                   (string) The transaction id.\n"
           "    \"walletconflicts\": [                       (array) Conflicting transaction ids.\n"
           "      \"txid\",                                  (string) The transaction id.\n"
           "      ...\n"
           "    ],\n"
           "    \"time\": xxx,                               (numeric) The transaction time expressed in " + UNIX_EPOCH_TIME + ".\n"
           "    \"timereceived\": xxx,                       (numeric) The time received expressed in " + UNIX_EPOCH_TIME + ".\n"
           "    \"comment\": \"...\",                          (string) If a comment is associated with the transaction, only present if not empty.\n"
           "    \"bip125-replaceable\": \"yes|no|unknown\",    (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
           "                                                     may be unknown for unconfirmed transactions not in the mempool\n";
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listtransactions",
                "\nIf a label name is provided, this will return only incoming transactions paying to addresses with the specified label.\n"
                "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, should be a valid label name to return only incoming transactions\n"
            "              with the specified label, or \"*\" to disable filtering and return all transactions."},
                    {"count", RPCArg::Type::NUM, /* default */ "10", "The number of transactions to return"},
                    {"skip", RPCArg::Type::NUM, /* default */ "0", "The number of transactions to skip"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Include transactions to watch-only addresses (see 'importaddress')"},
                },
                RPCResult{
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\": xxx, (bool) Only returns true if imported addresses were involved in transaction.\n"
            "    \"address\":\"address\",    (string) The bitcoin address of the transaction.\n"
            "    \"category\":               (string) The transaction category.\n"
            "                \"send\"                  Transactions sent.\n"
            "                \"receive\"               Non-coinbase transactions received.\n"
            "                \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                \"orphan\"                Orphaned coinbase transactions received.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
            "                                        for all other categories\n"
            "    \"label\": \"label\",       (string) A comment for the address/transaction, if any\n"
            "    \"vout\": n,                (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            + TransactionDescriptionString()
            + "    \"abandoned\": xxx          (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                         'send' category of transactions.\n"
            "  }\n"
            "]\n"
                },
                RPCExamples{
            "\nList the most recent 10 transactions in the systems\n"
            + HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n"
            + HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listtransactions", "\"*\", 20, 100")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    const std::string* filter_label = nullptr;
    if (!request.params[0].isNull() && request.params[0].get_str() != "*") {
        filter_label = &request.params[0].get_str();
        if (filter_label->empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Label argument must be a valid label name or \"*\".");
        }
    }
    int nCount = 10;
    if (!request.params[1].isNull())
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (!request.params[2].isNull())
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(request.params[3], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        const CWallet::TxItems & txOrdered = pwallet->wtxOrdered;

        // iterate backwards until we have nCount items to return:
        for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
        {
            CWalletTx *const pwtx = (*it).second;
            ListTransactions(*locked_chain, pwallet, *pwtx, 0, true, ret, filter, filter_label);
            if ((int)ret.size() >= (nCount+nFrom)) break;
        }
    }

    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

static UniValue listsinceblock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listsinceblock",
                "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted.\n"
                "If \"blockhash\" is no longer a part of the main chain, transactions from the fork point onward are included.\n"
                "Additionally, if include_removed is set, transactions affecting the wallet which were removed are returned in the \"removed\" array.\n",
                {
                    {"blockhash", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, the block hash to list transactions since, otherwise list all transactions."},
                    {"target_confirmations", RPCArg::Type::NUM, /* default */ "1", "Return the nth block hash from the main chain. e.g. 1 would mean the best block hash. Note: this is not used as a filter, but only affects [lastblock] in the return value"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Include transactions to watch-only addresses (see 'importaddress')"},
                    {"include_removed", RPCArg::Type::BOOL, /* default */ "true", "Show transactions that were removed due to a reorg in the \"removed\" array\n"
            "                                                           (not guaranteed to work on pruned nodes)"},
                },
                RPCResult{
            "{\n"
            "  \"transactions\": [\n"
            "    \"involvesWatchonly\": xxx, (bool) Only returns true if imported addresses were involved in transaction.\n"
            "    \"address\":\"address\",    (string) The bitcoin address of the transaction.\n"
            "    \"category\":               (string) The transaction category.\n"
            "                \"send\"                  Transactions sent.\n"
            "                \"receive\"               Non-coinbase transactions received.\n"
            "                \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                \"orphan\"                Orphaned coinbase transactions received.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
            "                                         for all other categories\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the 'send' category of transactions.\n"
            + TransactionDescriptionString()
            + "    \"abandoned\": xxx,         (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the 'send' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"label\" : \"label\"       (string) A comment for the address/transaction, if any\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
            "  ],\n"
            "  \"removed\": [\n"
            "    <structure is the same as \"transactions\" above, only present if include_removed=true>\n"
            "    Note: transactions that were re-added in the active chain will appear as-is in this array, and may thus have a positive confirmation count.\n"
            "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the block (target_confirmations-1) from the best block on the main chain. This is typically used to feed back into listsinceblock the next time you call it. So you would generally use a target_confirmations of say 6, so you will be continually re-notified of transactions until they've reached 6 confirmations plus any new ones\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("listsinceblock", "")
            + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6")
            + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // The way the 'height' is initialized is just a workaround for the gcc bug #47679 since version 4.6.0.
    Optional<int> height = MakeOptional(false, int()); // Height of the specified block or the common ancestor, if the block provided was in a deactivated chain.
    Optional<int> altheight; // Height of the specified block, even if it's in a deactivated chain.
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    uint256 blockId;
    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        blockId = ParseHashV(request.params[0], "blockhash");
        height = locked_chain->findFork(blockId, &altheight);
        if (!height) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    if (!request.params[1].isNull()) {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        }
    }

    if (ParseIncludeWatchonly(request.params[2], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool include_removed = (request.params[3].isNull() || request.params[3].get_bool());

    const Optional<int> tip_height = locked_chain->getHeight();
    int depth = tip_height && height ? (1 + *tip_height - *height) : -1;

    UniValue transactions(UniValue::VARR);

    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        CWalletTx tx = pairWtx.second;

        if (depth == -1 || abs(tx.GetDepthInMainChain()) < depth) {
            ListTransactions(*locked_chain, pwallet, tx, 0, true, transactions, filter, nullptr /* filter_label */);
        }
    }

    // when a reorg'd block is requested, we also list any relevant transactions
    // in the blocks of the chain that was detached
    UniValue removed(UniValue::VARR);
    while (include_removed && altheight && *altheight > *height) {
        CBlock block;
        if (!pwallet->chain().findBlock(blockId, &block) || block.IsNull()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }
        for (const CTransactionRef& tx : block.vtx) {
            auto it = pwallet->mapWallet.find(tx->GetHash());
            if (it != pwallet->mapWallet.end()) {
                // We want all transactions regardless of confirmation count to appear here,
                // even negative confirmation ones, hence the big negative.
                ListTransactions(*locked_chain, pwallet, it->second, -100000000, true, removed, filter, nullptr /* filter_label */);
            }
        }
        blockId = block.hashPrevBlock;
        --*altheight;
    }

    int last_height = tip_height ? *tip_height + 1 - target_confirms : -1;
    uint256 lastblock = last_height >= 0 ? locked_chain->getBlockHash(last_height) : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("transactions", transactions);
    if (include_removed) ret.pushKV("removed", removed);
    ret.pushKV("lastblock", lastblock.GetHex());

    return ret;
}

static UniValue gettransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"gettransaction",
                "\nGet detailed information about in-wallet transaction <txid>\n",
                {
                    {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false",
                            "Whether to include watch-only addresses in balance calculation and details[]"},
                    {"verbose", RPCArg::Type::BOOL, /* default */ "false",
                            "Whether to include a `decoded` field containing the decoded transaction (equivalent to RPC decoderawtransaction)"},
                },
                RPCResult{
            "{\n"
            "    \"amount\" : x.xxx,        (numeric) The transaction amount in " + CURRENCY_UNIT + "\n"
            "    \"fee\": x.xxx,            (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                              'send' category of transactions.\n"
            + TransactionDescriptionString()
            + "    \"details\" : [\n"
            "      {\n"
            "        \"involvesWatchonly\": xxx,         (bool) Only returns true if imported addresses were involved in transaction.\n"
            "        \"address\" : \"address\",          (string) The bitcoin address involved in the transaction\n"
            "        \"category\" :                      (string) The transaction category.\n"
            "                     \"send\"                  Transactions sent.\n"
            "                     \"receive\"               Non-coinbase transactions received.\n"
            "                     \"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
            "                     \"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
            "                     \"orphan\"                Orphaned coinbase transactions received.\n"
            "        \"amount\" : x.xxx,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "        \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
            "        \"vout\" : n,                       (numeric) the vout value\n"
            "        \"fee\": x.xxx,                     (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                           'send' category of transactions.\n"
            "        \"abandoned\": xxx                  (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                           'send' category of transactions.\n"
            "      }\n"
            "      ,...\n"
            "    ],\n"
            "    \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "    \"decoded\" : transaction         (json object) Optional, the decoded transaction (only present when `verbose` is passed), equivalent to the\n"
            "                                                  RPC decoderawtransaction method, or the RPC getrawtransaction method when `verbose` is passed.\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true")
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" false true")
            + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    uint256 hash(ParseHashV(request.params[0], "txid"));

    isminefilter filter = ISMINE_SPENDABLE;

    if (ParseIncludeWatchonly(request.params[1], *pwallet)) {
        filter |= ISMINE_WATCH_ONLY;
    }

    bool verbose = request.params[2].isNull() ? false : request.params[2].get_bool();

    UniValue entry(UniValue::VOBJ);
    auto it = pwallet->mapWallet.find(hash);
    if (it == pwallet->mapWallet.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    const CWalletTx& wtx = it->second;

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    entry.pushKV("amount", ValueFromAmount(nNet - nFee));
    if (wtx.IsFromMe(filter))
        entry.pushKV("fee", ValueFromAmount(nFee));

    WalletTxToJSON(pwallet->chain(), *locked_chain, wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(*locked_chain, pwallet, wtx, 0, false, details, filter, nullptr /* filter_label */);
    entry.pushKV("details", details);

    std::string strHex = EncodeHexTx(*wtx.tx, pwallet->chain().rpcSerializationFlags());
    entry.pushKV("hex", strHex);

    if (verbose) {
        UniValue decoded(UniValue::VOBJ);
        TxToUniv(*wtx.tx, uint256(), decoded, false);
        entry.pushKV("decoded", decoded);
    }

    return entry;
}

static UniValue abandontransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"abandontransaction",
                "\nMark in-wallet transaction <txid> as abandoned\n"
                "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
                "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
                "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
                "It has no effect on transactions which are already abandoned.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    uint256 hash(ParseHashV(request.params[0], "txid"));

    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    if (!pwallet->AbandonTransaction(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");
    }

    return NullUniValue;
}


static UniValue backupwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"backupwallet",
                "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n",
                {
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination directory or file"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("backupwallet", "\"backup.dat\"")
            + HelpExampleRpc("backupwallet", "\"backup.dat\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
}


static UniValue keypoolrefill(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"keypoolrefill",
                "\nFills the keypool."+
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newsize", RPCArg::Type::NUM, /* default */ "100", "The new keypool size"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("keypoolrefill", "")
            + HelpExampleRpc("keypoolrefill", "")
                },
            }.Check(request);

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (!request.params[0].isNull()) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < kpSize) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
    }

    return NullUniValue;
}


static UniValue walletpassphrase(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletpassphrase",
                "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
                "This is needed prior to performing transactions related to private keys such as sending bitcoins\n"
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet passphrase"},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The time to keep the decryption key in seconds; capped at 100000000 (~3 years)."},
                },
                RPCResults{},
                RPCExamples{
            "\nUnlock the wallet for 60 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nLock the wallet again (before 60 seconds)\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
    }

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    // Get the timeout
    int64_t nSleepTime = request.params[1].get_int64();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout
    constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
    if (nSleepTime > MAX_SLEEP_TIME) {
        nSleepTime = MAX_SLEEP_TIME;
    }

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->Unlock(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    pwallet->TopUpKeyPool();

    pwallet->nRelockTime = GetTime() + nSleepTime;

    // Keep a weak pointer to the wallet so that it is possible to unload the
    // wallet before the following callback is called. If a valid shared pointer
    // is acquired in the callback then the wallet is still loaded.
    std::weak_ptr<CWallet> weak_wallet = wallet;
    pwallet->chain().rpcRunLater(strprintf("lockwallet(%s)", pwallet->GetName()), [weak_wallet] {
        if (auto shared_wallet = weak_wallet.lock()) {
            LOCK(shared_wallet->cs_wallet);
            shared_wallet->Lock();
            shared_wallet->nRelockTime = 0;
        }
    }, nSleepTime);

    return NullUniValue;
}


static UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletpassphrasechange",
                "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n",
                {
                    {"oldpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The current passphrase"},
                    {"newpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The new passphrase"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
    }

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.empty() || strNewWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    return NullUniValue;
}


static UniValue walletlock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletlock",
                "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
                "After calling this method, you will need to call walletpassphrase again\n"
                "before being able to call any methods which require the wallet to be unlocked.\n",
                {},
                RPCResults{},
                RPCExamples{
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("walletlock", "")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
    }

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


static UniValue encryptwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"encryptwallet",
                "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
                "After this, any calls that interact with private keys such as sending or signing \n"
                "will require the passphrase to be set prior the making these calls.\n"
                "Use the walletpassphrase call for this, and then walletlock call.\n"
                "If the wallet is already encrypted, use the walletpassphrasechange call.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long."},
                },
                RPCResults{},
                RPCExamples{
            "\nEncrypt your wallet\n"
            + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending bitcoin\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can do something like sign\n"
            + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: wallet does not contain private keys, nothing to encrypt.");
    }

    if (pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
    }

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->EncryptWallet(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
    }

    return "wallet encrypted; The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

static UniValue lockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"lockunspent",
                "\nUpdates list of temporarily unspendable outputs.\n"
                "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
                "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
                "A locked transaction output will not be chosen by automatic coin selection, when spending bitcoins.\n"
                "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
                "is always cleared (by virtue of process exit) when a node stops or fails.\n"
                "Also see the listunspent call\n",
                {
                    {"unlock", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to unlock (true) or lock (false) the specified transactions"},
                    {"transactions", RPCArg::Type::ARR, /* default */ "empty array", "A json array of objects. Each object the txid (string) vout (numeric).",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
            "true|false    (boolean) Whether the command was successful or not\n"
                },
                RPCExamples{
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"")
                },
            }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    RPCTypeCheckArgument(request.params[0], UniValue::VBOOL);

    bool fUnlock = request.params[0].get_bool();

    if (request.params[1].isNull()) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    RPCTypeCheckArgument(request.params[1], UniValue::VARR);

    const UniValue& output_params = request.params[1];

    // Create and validate the COutPoints first.

    std::vector<COutPoint> outputs;
    outputs.reserve(output_params.size());

    for (unsigned int idx = 0; idx < output_params.size(); idx++) {
        const UniValue& o = output_params[idx].get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        const uint256 txid(ParseHashO(o, "txid"));
        const int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }

        const COutPoint outpt(txid, nOutput);

        const auto it = pwallet->mapWallet.find(outpt.hash);
        if (it == pwallet->mapWallet.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, unknown transaction");
        }

        const CWalletTx& trans = it->second;

        if (outpt.n >= trans.tx->vout.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout index out of bounds");
        }

        if (pwallet->IsSpent(outpt.hash, outpt.n)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected unspent output");
        }

        const bool is_locked = pwallet->IsLockedCoin(outpt.hash, outpt.n);

        if (fUnlock && !is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected locked output");
        }

        if (!fUnlock && is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output already locked");
        }

        outputs.push_back(outpt);
    }

    // Atomically set (un)locked status for the outputs.
    for (const COutPoint& outpt : outputs) {
        if (fUnlock) pwallet->UnlockCoin(outpt);
        else pwallet->LockCoin(outpt);
    }

    return true;
}

static UniValue listlockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listlockunspent",
                "\nReturns list of temporarily unspendable outputs.\n"
                "See the lockunspent call to lock and unlock transactions for spending.\n",
                {},
                RPCResult{
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listlockunspent", "")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::vector<COutPoint> vOutpts;
    pwallet->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    for (const COutPoint& outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.pushKV("txid", outpt.hash.GetHex());
        o.pushKV("vout", (int)outpt.n);
        ret.push_back(o);
    }

    return ret;
}

static UniValue settxfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"settxfee",
                "\nSet the transaction fee per kB for this wallet. Overrides the global -paytxfee command line parameter.\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The transaction fee in " + CURRENCY_UNIT + "/kB"},
                },
                RPCResult{
            "true|false        (boolean) Returns true if successful\n"
                },
                RPCExamples{
                    HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
                },
            }.Check(request);

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    CAmount nAmount = AmountFromValue(request.params[0]);
    CFeeRate tx_fee_rate(nAmount, 1000);
    if (tx_fee_rate == CFeeRate(0)) {
        // automatic selection
    } else if (tx_fee_rate < pwallet->chain().relayMinFee()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than min relay tx fee (%s)", pwallet->chain().relayMinFee().ToString()));
    } else if (tx_fee_rate < pwallet->m_min_fee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than wallet min fee (%s)", pwallet->m_min_fee.ToString()));
    }

    pwallet->m_pay_tx_fee = tx_fee_rate;
    return true;
}

static UniValue getbalances(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const rpc_wallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(rpc_wallet.get(), request.fHelp)) {
        return NullUniValue;
    }
    CWallet& wallet = *rpc_wallet;

    RPCHelpMan{
        "getbalances",
        "Returns an object with all balances in " + CURRENCY_UNIT + ".\n",
        {},
        RPCResult{
            "{\n"
            "    \"mine\": {                        (object) balances from outputs that the wallet can sign\n"
            "      \"trusted\": xxx                 (numeric) trusted balance (outputs created by the wallet or confirmed outputs)\n"
            "      \"untrusted_pending\": xxx       (numeric) untrusted pending balance (outputs created by others that are in the mempool)\n"
            "      \"immature\": xxx                (numeric) balance from immature coinbase outputs\n"
            "      \"used\": xxx                    (numeric) (only present if avoid_reuse is set) balance from coins sent to addresses that were previously spent from (potentially privacy violating)\n"
            "    },\n"
            "    \"watchonly\": {                   (object) watchonly balances (not present if wallet does not watch anything)\n"
            "      \"trusted\": xxx                 (numeric) trusted balance (outputs created by the wallet or confirmed outputs)\n"
            "      \"untrusted_pending\": xxx       (numeric) untrusted pending balance (outputs created by others that are in the mempool)\n"
            "      \"immature\": xxx                (numeric) balance from immature coinbase outputs\n"
            "    },\n"
            "}\n"},
        RPCExamples{
            HelpExampleCli("getbalances", "") +
            HelpExampleRpc("getbalances", "")},
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    auto locked_chain = wallet.chain().lock();
    LOCK(wallet.cs_wallet);

    UniValue obj(UniValue::VOBJ);

    const auto bal = wallet.GetBalance();
    UniValue balances{UniValue::VOBJ};
    {
        UniValue balances_mine{UniValue::VOBJ};
        balances_mine.pushKV("trusted", ValueFromAmount(bal.m_mine_trusted));
        balances_mine.pushKV("untrusted_pending", ValueFromAmount(bal.m_mine_untrusted_pending));
        balances_mine.pushKV("immature", ValueFromAmount(bal.m_mine_immature));
        if (wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE)) {
            // If the AVOID_REUSE flag is set, bal has been set to just the un-reused address balance. Get
            // the total balance, and then subtract bal to get the reused address balance.
            const auto full_bal = wallet.GetBalance(0, false);
            balances_mine.pushKV("used", ValueFromAmount(full_bal.m_mine_trusted + full_bal.m_mine_untrusted_pending - bal.m_mine_trusted - bal.m_mine_untrusted_pending));
        }
        balances.pushKV("mine", balances_mine);
    }
    auto spk_man = wallet.GetLegacyScriptPubKeyMan();
    if (spk_man && spk_man->HaveWatchOnly()) {
        UniValue balances_watchonly{UniValue::VOBJ};
        balances_watchonly.pushKV("trusted", ValueFromAmount(bal.m_watchonly_trusted));
        balances_watchonly.pushKV("untrusted_pending", ValueFromAmount(bal.m_watchonly_untrusted_pending));
        balances_watchonly.pushKV("immature", ValueFromAmount(bal.m_watchonly_immature));
        balances.pushKV("watchonly", balances_watchonly);
    }
    return balances;
}

static UniValue getwalletinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"getwalletinfo",
                "Returns an object containing various wallet state info.\n",
                {},
                RPCResult{
            "{\n"
            "  \"walletname\": xxxxx,               (string) the wallet name\n"
            "  \"walletversion\": xxxxx,            (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,                (numeric) DEPRECATED. Identical to getbalances().mine.trusted\n"
            "  \"unconfirmed_balance\": xxx,        (numeric) DEPRECATED. Identical to getbalances().mine.untrusted_pending\n"
            "  \"immature_balance\": xxxxxx,        (numeric) DEPRECATED. Identical to getbalances().mine.immature\n"
            "  \"txcount\": xxxxxxx,                (numeric) the total number of transactions in the wallet\n"
            "  \"keypoololdest\": xxxxxx,           (numeric) the " + UNIX_EPOCH_TIME + " of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,               (numeric) how many new keys are pre-generated (only counts external keys)\n"
            "  \"keypoolsize_hd_internal\": xxxx,   (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
            "  \"unlocked_until\": ttt,             (numeric) the " + UNIX_EPOCH_TIME + " until which the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,                (numeric) the transaction fee configuration, set in " + CURRENCY_UNIT + "/kB\n"
            "  \"hdseedid\": \"<hash160>\"            (string, optional) the Hash160 of the HD seed (only present when HD is enabled)\n"
            "  \"private_keys_enabled\": true|false (boolean) false if privatekeys are disabled for this wallet (enforced watch-only wallet)\n"
            "  \"avoid_reuse\": true|false          (boolean) whether this wallet tracks clean/dirty coins in terms of reuse\n"
            "  \"scanning\":                        (json object) current scanning details, or false if no scan is in progress\n"
            "    {\n"
            "      \"duration\" : xxxx              (numeric) elapsed seconds since scan start\n"
            "      \"progress\" : x.xxxx,           (numeric) scanning progress percentage [0.0, 1.0]\n"
            "    }\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getwalletinfo", "")
            + HelpExampleRpc("getwalletinfo", "")
                },
    }.Check(request);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    UniValue obj(UniValue::VOBJ);

    size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
    const auto bal = pwallet->GetBalance();
    obj.pushKV("walletname", pwallet->GetName());
    obj.pushKV("walletversion", pwallet->GetVersion());
    obj.pushKV("balance", ValueFromAmount(bal.m_mine_trusted));
    obj.pushKV("unconfirmed_balance", ValueFromAmount(bal.m_mine_untrusted_pending));
    obj.pushKV("immature_balance", ValueFromAmount(bal.m_mine_immature));
    obj.pushKV("txcount",       (int)pwallet->mapWallet.size());
    obj.pushKV("keypoololdest", pwallet->GetOldestKeyPoolTime());
    obj.pushKV("keypoolsize", (int64_t)kpExternalSize);

    LegacyScriptPubKeyMan* spk_man = pwallet->GetLegacyScriptPubKeyMan();
    if (spk_man) {
        CKeyID seed_id = spk_man->GetHDChain().seed_id;
        if (!seed_id.IsNull()) {
            obj.pushKV("hdseedid", seed_id.GetHex());
        }
    }

    if (pwallet->CanSupportFeature(FEATURE_HD_SPLIT)) {
        obj.pushKV("keypoolsize_hd_internal",   (int64_t)(pwallet->GetKeyPoolSize() - kpExternalSize));
    }
    if (pwallet->IsCrypted()) {
        obj.pushKV("unlocked_until", pwallet->nRelockTime);
    }
    obj.pushKV("paytxfee", ValueFromAmount(pwallet->m_pay_tx_fee.GetFeePerK()));
    obj.pushKV("private_keys_enabled", !pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));
    obj.pushKV("avoid_reuse", pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE));
    if (pwallet->IsScanning()) {
        UniValue scanning(UniValue::VOBJ);
        scanning.pushKV("duration", pwallet->ScanningDuration() / 1000);
        scanning.pushKV("progress", pwallet->ScanningProgress());
        obj.pushKV("scanning", scanning);
    } else {
        obj.pushKV("scanning", false);
    }
    return obj;
}

static UniValue listwalletdir(const JSONRPCRequest& request)
{
            RPCHelpMan{"listwalletdir",
                "Returns a list of wallets in the wallet directory.\n",
                {},
                RPCResult{
            "{\n"
            "  \"wallets\" : [                (json array of objects)\n"
            "    {\n"
            "      \"name\" : \"name\"          (string) The wallet name\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("listwalletdir", "")
            + HelpExampleRpc("listwalletdir", "")
                },
            }.Check(request);

    UniValue wallets(UniValue::VARR);
    for (const auto& path : ListWalletDir()) {
        UniValue wallet(UniValue::VOBJ);
        wallet.pushKV("name", path.string());
        wallets.push_back(wallet);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("wallets", wallets);
    return result;
}

static UniValue listwallets(const JSONRPCRequest& request)
{
            RPCHelpMan{"listwallets",
                "Returns a list of currently loaded wallets.\n"
                "For full information on the wallet, use \"getwalletinfo\"\n",
                {},
                RPCResult{
            "[                         (json array of strings)\n"
            "  \"walletname\"            (string) the wallet name\n"
            "   ...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listwallets", "")
            + HelpExampleRpc("listwallets", "")
                },
            }.Check(request);

    UniValue obj(UniValue::VARR);

    for (const std::shared_ptr<CWallet>& wallet : GetWallets()) {
        if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
            return NullUniValue;
        }

        LOCK(wallet->cs_wallet);

        obj.push_back(wallet->GetName());
    }

    return obj;
}

static UniValue loadwallet(const JSONRPCRequest& request)
{
            RPCHelpMan{"loadwallet",
                "\nLoads a wallet from a wallet file or directory."
                "\nNote that all wallet command-line options used when starting bitcoind will be"
                "\napplied to the new wallet (eg -zapwallettxes, upgradewallet, rescan, etc).\n",
                {
                    {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet directory or .dat file."},
                },
                RPCResult{
            "{\n"
            "  \"name\" :    <wallet_name>,        (string) The wallet name if loaded successfully.\n"
            "  \"warning\" : <warning>,            (string) Warning message if wallet was not loaded cleanly.\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("loadwallet", "\"test.dat\"")
            + HelpExampleRpc("loadwallet", "\"test.dat\"")
                },
            }.Check(request);

    WalletLocation location(request.params[0].get_str());

    if (!location.Exists()) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet " + location.GetName() + " not found.");
    } else if (fs::is_directory(location.GetPath())) {
        // The given filename is a directory. Check that there's a wallet.dat file.
        fs::path wallet_dat_file = location.GetPath() / "wallet.dat";
        if (fs::symlink_status(wallet_dat_file).type() == fs::file_not_found) {
            throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Directory " + location.GetName() + " does not contain a wallet.dat file.");
        }
    }

    std::string error;
    std::vector<std::string> warning;
    std::shared_ptr<CWallet> const wallet = LoadWallet(*g_rpc_chain, location, error, warning);
    if (!wallet) throw JSONRPCError(RPC_WALLET_ERROR, error);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", Join(warning, "\n"));

    return obj;
}

static UniValue setwalletflag(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            std::string flags = "";
            for (auto& it : WALLET_FLAG_MAP)
                if (it.second & MUTABLE_WALLET_FLAGS)
                    flags += (flags == "" ? "" : ", ") + it.first;
            RPCHelpMan{"setwalletflag",
                "\nChange the state of the given wallet flag for a wallet.\n",
                {
                    {"flag", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of the flag to change. Current available flags: " + flags},
                    {"value", RPCArg::Type::BOOL, /* default */ "true", "The new state."},
                },
                RPCResult{
            "{\n"
            "    \"flag_name\": string   (string) The name of the flag that was modified\n"
            "    \"flag_state\": bool    (bool) The new state of the flag\n"
            "    \"warnings\": string    (string) Any warnings associated with the change\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("setwalletflag", "avoid_reuse")
                  + HelpExampleRpc("setwalletflag", "\"avoid_reuse\"")
                },
            }.Check(request);

    std::string flag_str = request.params[0].get_str();
    bool value = request.params[1].isNull() || request.params[1].get_bool();

    if (!WALLET_FLAG_MAP.count(flag_str)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unknown wallet flag: %s", flag_str));
    }

    auto flag = WALLET_FLAG_MAP.at(flag_str);

    if (!(flag & MUTABLE_WALLET_FLAGS)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Wallet flag is immutable: %s", flag_str));
    }

    UniValue res(UniValue::VOBJ);

    if (pwallet->IsWalletFlagSet(flag) == value) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Wallet flag is already set to %s: %s", value ? "true" : "false", flag_str));
    }

    res.pushKV("flag_name", flag_str);
    res.pushKV("flag_state", value);

    if (value) {
        pwallet->SetWalletFlag(flag);
    } else {
        pwallet->UnsetWalletFlag(flag);
    }

    if (flag && value && WALLET_FLAG_CAVEATS.count(flag)) {
        res.pushKV("warnings", WALLET_FLAG_CAVEATS.at(flag));
    }

    return res;
}

static UniValue createwallet(const JSONRPCRequest& request)
{
    RPCHelpMan{
        "createwallet",
        "\nCreates and loads a new wallet.\n",
        {
            {"wallet_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name for the new wallet. If this is a path, the wallet will be created at the path location."},
            {"disable_private_keys", RPCArg::Type::BOOL, /* default */ "false", "Disable the possibility of private keys (only watchonlys are possible in this mode)."},
            {"blank", RPCArg::Type::BOOL, /* default */ "false", "Create a blank wallet. A blank wallet has no keys or HD seed. One can be set using sethdseed."},
            {"passphrase", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Encrypt the wallet with this passphrase."},
            {"avoid_reuse", RPCArg::Type::BOOL, /* default */ "false", "Keep track of coin reuse, and treat dirty and clean coins differently with privacy considerations in mind."},
        },
        RPCResult{
            "{\n"
            "  \"name\" :    <wallet_name>,        (string) The wallet name if created successfully. If the wallet was created using a full path, the wallet_name will be the full path.\n"
            "  \"warning\" : <warning>,            (string) Warning message if wallet was not loaded cleanly.\n"
            "}\n"
        },
        RPCExamples{
            HelpExampleCli("createwallet", "\"testwallet\"")
            + HelpExampleRpc("createwallet", "\"testwallet\"")
        },
    }.Check(request);

    uint64_t flags = 0;
    if (!request.params[1].isNull() && request.params[1].get_bool()) {
        flags |= WALLET_FLAG_DISABLE_PRIVATE_KEYS;
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        flags |= WALLET_FLAG_BLANK_WALLET;
    }
    SecureString passphrase;
    passphrase.reserve(100);
    std::vector<std::string> warnings;
    if (!request.params[3].isNull()) {
        passphrase = request.params[3].get_str().c_str();
        if (passphrase.empty()) {
            // Empty string means unencrypted
            warnings.emplace_back("Empty string given as passphrase, wallet will not be encrypted.");
        }
    }

    if (!request.params[4].isNull() && request.params[4].get_bool()) {
        flags |= WALLET_FLAG_AVOID_REUSE;
    }

    std::string error;
    std::shared_ptr<CWallet> wallet;
    WalletCreationStatus status = CreateWallet(*g_rpc_chain, passphrase, flags, request.params[0].get_str(), error, warnings, wallet);
    switch (status) {
        case WalletCreationStatus::CREATION_FAILED:
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        case WalletCreationStatus::ENCRYPTION_FAILED:
            throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, error);
        case WalletCreationStatus::SUCCESS:
            break;
        // no default case, so the compiler can warn about missing cases
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", Join(warnings, "\n"));

    return obj;
}

static UniValue unloadwallet(const JSONRPCRequest& request)
{
            RPCHelpMan{"unloadwallet",
                "Unloads the wallet referenced by the request endpoint otherwise unloads the wallet specified in the argument.\n"
                "Specifying the wallet name on a wallet endpoint is invalid.",
                {
                    {"wallet_name", RPCArg::Type::STR, /* default */ "the wallet name from the RPC request", "The name of the wallet to unload."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("unloadwallet", "wallet_name")
            + HelpExampleRpc("unloadwallet", "wallet_name")
                },
            }.Check(request);

    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        if (!request.params[0].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot unload the requested wallet");
        }
    } else {
        wallet_name = request.params[0].get_str();
    }

    std::shared_ptr<CWallet> wallet = GetWallet(wallet_name);
    if (!wallet) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }

    // Release the "main" shared pointer and prevent further notifications.
    // Note that any attempt to load the same wallet would fail until the wallet
    // is destroyed (see CheckUniqueFileid).
    if (!RemoveWallet(wallet)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Requested wallet already unloaded");
    }

    UnloadWallet(std::move(wallet));

    return NullUniValue;
}

static UniValue listunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{
                "listunspent",
                "\nReturns array of unspent transaction outputs\n"
                "with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include txouts paid to specified addresses.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum confirmations to filter"},
                    {"maxconf", RPCArg::Type::NUM, /* default */ "9999999", "The maximum confirmations to filter"},
                    {"addresses", RPCArg::Type::ARR, /* default */ "empty array", "A json array of bitcoin addresses to filter",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "bitcoin address"},
                        },
                    },
                    {"include_unsafe", RPCArg::Type::BOOL, /* default */ "true", "Include outputs that are not safe to spend\n"
            "                  See description of \"safe\" attribute below."},
                    {"query_options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "JSON with query options",
                        {
                            {"minimumAmount", RPCArg::Type::AMOUNT, /* default */ "0", "Minimum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Maximum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumCount", RPCArg::Type::NUM, /* default */ "unlimited", "Maximum number of UTXOs"},
                            {"minimumSumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Minimum sum value of all UTXOs in " + CURRENCY_UNIT + ""},
                        },
                        "query_options"},
                },
                RPCResult{
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the bitcoin address\n"
            "    \"label\" : \"label\",        (string) The associated label, or \"\" for the default label\n"
            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            "    \"redeemScript\" : \"script\" (string) The redeemScript if scriptPubKey is P2SH\n"
            "    \"witnessScript\" : \"script\" (string) witnessScript if the scriptPubKey is P2WSH or P2SH-P2WSH\n"
            "    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            "    \"solvable\" : xxx,         (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "    \"reused\" : xxx,           (bool) (only present if avoid_reuse is set) Whether this output is reused/dirty (sent to an address that was previously spent from)\n"
            "    \"desc\" : xxx,             (string, only when solvable) A descriptor for spending this output\n"
            "    \"safe\" : xxx              (bool) Whether this output is considered safe to spend. Unconfirmed transactions\n"
            "                              from outside keys and unconfirmed replacement transactions are considered unsafe\n"
            "                              and are not eligible for spending by fundrawtransaction and sendtoaddress.\n"
            "  }\n"
            "  ,...\n"
            "]\n"
                },
                RPCExamples{
                    HelpExampleCli("listunspent", "")
            + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleCli("listunspent", "6 9999999 '[]' true '{ \"minimumAmount\": 0.005 }'")
            + HelpExampleRpc("listunspent", "6, 9999999, [] , true, { \"minimumAmount\": 0.005 } ")
                },
            }.Check(request);

    int nMinDepth = 1;
    if (!request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (!request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CTxDestination> destinations;
    if (!request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CTxDestination dest = DecodeDestination(input.get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Bitcoin address: ") + input.get_str());
            }
            if (!destinations.insert(dest).second) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            }
        }
    }

    bool include_unsafe = true;
    if (!request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;

    if (!request.params[4].isNull()) {
        const UniValue& options = request.params[4].get_obj();

        if (options.exists("minimumAmount"))
            nMinimumAmount = AmountFromValue(options["minimumAmount"]);

        if (options.exists("maximumAmount"))
            nMaximumAmount = AmountFromValue(options["maximumAmount"]);

        if (options.exists("minimumSumAmount"))
            nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);

        if (options.exists("maximumCount"))
            nMaximumCount = options["maximumCount"].get_int64();
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;
    {
        CCoinControl cctl;
        cctl.m_avoid_address_reuse = false;
        cctl.m_min_depth = nMinDepth;
        cctl.m_max_depth = nMaxDepth;
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);
        pwallet->AvailableCoins(*locked_chain, vecOutputs, !include_unsafe, &cctl, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount);
    }

    LOCK(pwallet->cs_wallet);

    const bool avoid_reuse = pwallet->IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);

    for (const COutput& out : vecOutputs) {
        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);
        bool reused = avoid_reuse && pwallet->IsUsedDestination(out.tx->GetHash(), out.i);

        if (destinations.size() && (!fValidAddress || !destinations.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", out.tx->GetHash().GetHex());
        entry.pushKV("vout", out.i);

        if (fValidAddress) {
            entry.pushKV("address", EncodeDestination(address));

            auto i = pwallet->mapAddressBook.find(address);
            if (i != pwallet->mapAddressBook.end()) {
                entry.pushKV("label", i->second.name);
            }

            std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(scriptPubKey);
            if (provider) {
                if (scriptPubKey.IsPayToScriptHash()) {
                    const CScriptID& hash = CScriptID(boost::get<ScriptHash>(address));
                    CScript redeemScript;
                    if (provider->GetCScript(hash, redeemScript)) {
                        entry.pushKV("redeemScript", HexStr(redeemScript.begin(), redeemScript.end()));
                        // Now check if the redeemScript is actually a P2WSH script
                        CTxDestination witness_destination;
                        if (redeemScript.IsPayToWitnessScriptHash()) {
                            bool extracted = ExtractDestination(redeemScript, witness_destination);
                            CHECK_NONFATAL(extracted);
                            // Also return the witness script
                            const WitnessV0ScriptHash& whash = boost::get<WitnessV0ScriptHash>(witness_destination);
                            CScriptID id;
                            CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                            CScript witnessScript;
                            if (provider->GetCScript(id, witnessScript)) {
                                entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                            }
                        }
                    }
                } else if (scriptPubKey.IsPayToWitnessScriptHash()) {
                    const WitnessV0ScriptHash& whash = boost::get<WitnessV0ScriptHash>(address);
                    CScriptID id;
                    CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                    CScript witnessScript;
                    if (provider->GetCScript(id, witnessScript)) {
                        entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                    }
                }
            }
        }

        entry.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));
        entry.pushKV("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue));
        entry.pushKV("confirmations", out.nDepth);
        entry.pushKV("spendable", out.fSpendable);
        entry.pushKV("solvable", out.fSolvable);
        if (out.fSolvable) {
            std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(scriptPubKey);
            if (provider) {
                auto descriptor = InferDescriptor(scriptPubKey, *provider);
                entry.pushKV("desc", descriptor->ToString());
            }
        }
        if (avoid_reuse) entry.pushKV("reused", reused);
        entry.pushKV("safe", out.fSafe);
        results.push_back(entry);
    }

    return results;
}

void FundTransaction(CWallet* const pwallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, UniValue options)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CCoinControl coinControl;
    change_position = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!options.isNull()) {
      if (options.type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        coinControl.fAllowWatchOnly = options.get_bool();
      }
      else {
        RPCTypeCheckArgument(options, UniValue::VOBJ);
        RPCTypeCheckObj(options,
            {
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"feeRate", UniValueType()}, // will be checked below
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("changeAddress")) {
            CTxDestination dest = DecodeDestination(options["changeAddress"].get_str());

            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "changeAddress must be a valid bitcoin address");
            }

            coinControl.destChange = dest;
        }

        if (options.exists("changePosition"))
            change_position = options["changePosition"].get_int();

        if (options.exists("change_type")) {
            if (options.exists("changeAddress")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both changeAddress and address_type options");
            }
            coinControl.m_change_type = pwallet->m_default_change_type;
            if (!ParseOutputType(options["change_type"].get_str(), *coinControl.m_change_type)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(options["includeWatching"], *pwallet);

        if (options.exists("lockUnspents"))
            lockUnspents = options["lockUnspents"].get_bool();

        if (options.exists("feeRate"))
        {
            coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
            coinControl.fOverrideFeeRate = true;
        }

        if (options.exists("subtractFeeFromOutputs"))
            subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();

        if (options.exists("replaceable")) {
            coinControl.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("conf_target")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");
            }
            coinControl.m_confirm_target = ParseConfirmTarget(options["conf_target"], pwallet->chain().estimateMaxBlocks());
        }
        if (options.exists("estimate_mode")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
            }
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coinControl.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
      }
    } else {
        // if options is null and not a bool
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(NullUniValue, *pwallet);
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (change_position != -1 && (change_position < 0 || (unsigned int)change_position > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    std::string strFailReason;

    if (!pwallet->FundTransaction(tx, fee_out, change_position, strFailReason, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }
}

static UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    RPCHelpMan{"fundrawtransaction",
                "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
                "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
                "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                "The inputs added will not be signed, use signrawtransactionwithkey\n"
                " or signrawtransactionwithwallet for that.\n"
                "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
                "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n",
                {
                    {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}",
                        {
                            {"changeAddress", RPCArg::Type::STR, /* default */ "pool address", "The bitcoin address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only.\n"
                                                          "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                                          "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, /* default */ "empty array", "A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"iswitness", RPCArg::Type::BOOL, /* default */ "depends on heuristic tests", "Whether the transaction hex is a serialized witness transaction.\n"
                        "If iswitness is not present, heuristic tests will be used in decoding.\n"
                        "If true, only witness deserialization will be tried.\n"
                        "If false, only non-witness deserialization will be tried.\n"
                        "This boolean should reflect whether the transaction has inputs\n"
                        "(e.g. fully valid, or on-chain transactions), if known by the caller."
                    },
                },
                RPCResult{
                            "{\n"
                            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransactionwithwallet", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                                },
    }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValueType(), UniValue::VBOOL});

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CAmount fee;
    int change_position;
    FundTransaction(pwallet, tx, fee, change_position, request.params[1]);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);

    return result;
}

UniValue signrawtransactionwithwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"signrawtransactionwithwallet",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"prevtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of previous dependent transaction outputs",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"scriptPubKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "script key"},
                                    {"redeemScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witnessScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH or P2SH-P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "(required for Segwit inputs) the amount spent"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
            "{\n"
            "  \"hex\" : \"value\",                  (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,          (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                      (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",              (string) The hash of the referenced, previous transaction\n"
            "      \"vout\" : n,                   (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",          (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,               (numeric) Script sequence number\n"
            "      \"error\" : \"text\"              (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithwallet", "\"myhex\"")
            + HelpExampleRpc("signrawtransactionwithwallet", "\"myhex\"")
                },
            }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str(), true)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    // Sign the transaction
    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);

    // Fetch previous transactions (inputs):
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : mtx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    pwallet->chain().findCoins(coins);

    // Parse the prevtxs array
    ParsePrevouts(request.params[1], nullptr, coins);

    std::set<std::shared_ptr<SigningProvider>> providers;
    for (const std::pair<COutPoint, Coin> coin_pair : coins) {
        std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(coin_pair.second.out.scriptPubKey);
        if (provider) {
            providers.insert(std::move(provider));
        }
    }
    if (providers.size() == 0) {
        // When there are no available providers, use a dummy SigningProvider so we can check if the tx is complete
        providers.insert(std::make_shared<SigningProvider>());
    }

    UniValue result(UniValue::VOBJ);
    for (std::shared_ptr<SigningProvider> provider : providers) {
        SignTransaction(mtx, provider.get(), coins, request.params[2], result);
    }
     return result;
}

static UniValue bumpfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();


    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

            RPCHelpMan{"bumpfee",
                "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
                "An opt-in RBF transaction with the given txid must be in the wallet.\n"
                "The command will pay the additional fee by reducing change outputs or adding inputs when necessary. It may add a new change output if one does not already exist.\n"
                "If `totalFee` (DEPRECATED) is given, adding inputs is not supported, so there must be a single change output that is big enough or it will fail.\n"
                "All inputs in the original transaction will be included in the replacement transaction.\n"
                "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
                "By default, the new fee will be calculated automatically using estimatesmartfee.\n"
                "The user can specify a confirmation target for estimatesmartfee.\n"
                "Alternatively, the user can specify totalFee (DEPRECATED), or fee_rate (" + CURRENCY_UNIT + " per kB) for the new transaction .\n"
                "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
                "returned by getnetworkinfo) to enter the node's mempool.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid to be bumped"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"confTarget", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                            {"totalFee", RPCArg::Type::NUM, /* default */ "fallback to 'confTarget'", "Total fee (NOT feerate) to pay, in satoshis. (DEPRECATED)\n"
            "                         In rare cases, the actual fee paid might be slightly higher than the specified\n"
            "                         totalFee if the tx change output has to be removed because it is too close to\n"
            "                         the dust threshold."},
                            {"fee_rate", RPCArg::Type::NUM, /* default */ "fallback to 'confTarget'", "FeeRate (NOT total fee) to pay, in " + CURRENCY_UNIT + " per kB\n"
            "                         Specify a fee rate instead of relying on the built-in fee estimator.\n"
            "                         Must be at least 0.0001 BTC per kB higher than the current transaction fee rate.\n"},
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "true", "Whether the new transaction should still be\n"
            "                         marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
            "                         be left unchanged from the original. If false, any input sequence numbers in the\n"
            "                         original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
            "                         so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
            "                         still be replaceable in practice, for example if it has unconfirmed ancestors which\n"
            "                         are replaceable)."},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "         \"UNSET\"\n"
            "         \"ECONOMICAL\"\n"
            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                },
                RPCResult{
            "{\n"
            "  \"psbt\":    \"psbt\",    (string) The base64-encoded unsigned PSBT of the new transaction. Only returned when wallet private keys are disabled.\n"
            "  \"txid\":    \"value\",   (string) The id of the new transaction. Only returned when wallet private keys are enabled.\n"
            "  \"origfee\":  n,        (numeric) The fee of the replaced transaction.\n"
            "  \"fee\":      n,        (numeric) The fee of the new transaction.\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty).\n"
            "}\n"
                },
                RPCExamples{
            "\nBump the fee, get the new transaction\'s txid\n" +
                    HelpExampleCli("bumpfee", "<txid>")
                },
            }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash(ParseHashV(request.params[0], "txid"));

    CCoinControl coin_control;
    coin_control.fAllowWatchOnly = pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    // optional parameters
    CAmount totalFee = 0;
    coin_control.m_signal_bip125_rbf = true;

    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"totalFee", UniValueType(UniValue::VNUM)},
                {"fee_rate", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);
        if (options.exists("confTarget") && (options.exists("totalFee") || options.exists("fee_rate"))) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget can't be set with totalFee or fee_rate. Please provide either a confirmation target in blocks for automatic fee estimation, or an explicit fee rate.");
        } else if (options.exists("fee_rate") && options.exists("totalFee")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_rate can't be set along with totalFee.");
        } else if (options.exists("confTarget")) { // TODO: alias this to conf_target
            coin_control.m_confirm_target = ParseConfirmTarget(options["confTarget"], pwallet->chain().estimateMaxBlocks());
        } else if (options.exists("totalFee")) {
            if (!pwallet->chain().rpcEnableDeprecated("totalFee")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "totalFee argument has been deprecated and will be removed in 0.20. Please use -deprecatedrpc=totalFee to continue using this argument until removal.");
            }
            totalFee = options["totalFee"].get_int64();
            if (totalFee <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid totalFee %s (must be greater than 0)", FormatMoney(totalFee)));
            }
        } else if (options.exists("fee_rate")) {
            CFeeRate fee_rate(AmountFromValue(options["fee_rate"]));
            if (fee_rate <= CFeeRate(0)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid fee_rate %s (must be greater than 0)", fee_rate.ToString()));
            }
            coin_control.m_feerate = fee_rate;
        }

        if (options.exists("replaceable")) {
            coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("estimate_mode")) {
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coin_control.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);


    std::vector<std::string> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res;
    if (totalFee > 0) {
        // Targeting total fee bump. Requires a change output of sufficient size.
        res = feebumper::CreateTotalBumpTransaction(pwallet, hash, coin_control, totalFee, errors, old_fee, new_fee, mtx);
    } else {
        // Targeting feerate bump.
        res = feebumper::CreateRateBumpTransaction(*pwallet, hash, coin_control, errors, old_fee, new_fee, mtx);
    }
    if (res != feebumper::Result::OK) {
        switch(res) {
            case feebumper::Result::INVALID_ADDRESS_OR_KEY:
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0]);
                break;
            case feebumper::Result::INVALID_REQUEST:
                throw JSONRPCError(RPC_INVALID_REQUEST, errors[0]);
                break;
            case feebumper::Result::INVALID_PARAMETER:
                throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0]);
                break;
            case feebumper::Result::WALLET_ERROR:
                throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
                break;
            default:
                throw JSONRPCError(RPC_MISC_ERROR, errors[0]);
                break;
        }
    }

    UniValue result(UniValue::VOBJ);

    // If wallet private keys are enabled, return the new transaction id,
    // otherwise return the base64-encoded unsigned PSBT of the new transaction.
    if (!pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        if (!feebumper::SignTransaction(*pwallet, mtx)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
        }

        uint256 txid;
        if (feebumper::CommitTransaction(*pwallet, hash, std::move(mtx), errors, txid) != feebumper::Result::OK) {
            throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
        }

        result.pushKV("txid", txid.GetHex());
    } else {
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const TransactionError err = FillPSBT(pwallet, psbtx, complete, SIGHASH_ALL, false /* sign */, true /* bip32derivs */);
        CHECK_NONFATAL(err == TransactionError::OK);
        CHECK_NONFATAL(!complete);
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    result.pushKV("origfee", ValueFromAmount(old_fee));
    result.pushKV("fee", ValueFromAmount(new_fee));
    UniValue result_errors(UniValue::VARR);
    for (const std::string& error : errors) {
        result_errors.push_back(error);
    }
    result.pushKV("errors", result_errors);

    return result;
}

UniValue rescanblockchain(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"rescanblockchain",
                "\nRescan the local blockchain for wallet related transactions.\n"
                "Note: Use \"getwalletinfo\" to query the scanning progress.\n",
                {
                    {"start_height", RPCArg::Type::NUM, /* default */ "0", "block height where the rescan should start"},
                    {"stop_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "the last block height that should be scanned. If none is provided it will rescan up to the tip at return time of this call."},
                },
                RPCResult{
            "{\n"
            "  \"start_height\"     (numeric) The block height where the rescan started (the requested height or 0)\n"
            "  \"stop_height\"      (numeric) The height of the last rescanned block. May be null in rare cases if there was a reorg and the call didn't scan any blocks because they were already scanned in the background.\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("rescanblockchain", "100000 120000")
            + HelpExampleRpc("rescanblockchain", "100000, 120000")
                },
            }.Check(request);

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int start_height = 0;
    uint256 start_block, stop_block;
    {
        auto locked_chain = pwallet->chain().lock();
        Optional<int> tip_height = locked_chain->getHeight();

        if (!request.params[0].isNull()) {
            start_height = request.params[0].get_int();
            if (start_height < 0 || !tip_height || start_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid start_height");
            }
        }

        Optional<int> stop_height;
        if (!request.params[1].isNull()) {
            stop_height = request.params[1].get_int();
            if (*stop_height < 0 || !tip_height || *stop_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid stop_height");
            }
            else if (*stop_height < start_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "stop_height must be greater than start_height");
            }
        }

        // We can't rescan beyond non-pruned blocks, stop and throw an error
        if (locked_chain->findPruned(start_height, stop_height)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Can't rescan beyond pruned data. Use RPC call getblockchaininfo to determine your pruned height.");
        }

        if (tip_height) {
            start_block = locked_chain->getBlockHash(start_height);
            // If called with a stop_height, set the stop_height here to
            // trigger a rescan to that height.
            // If called without a stop height, leave stop_height as null here
            // so rescan continues to the tip (even if the tip advances during
            // rescan).
            if (stop_height) {
                stop_block = locked_chain->getBlockHash(*stop_height);
            }
        }
    }

    CWallet::ScanResult result =
        pwallet->ScanForWalletTransactions(start_block, stop_block, reserver, true /* fUpdate */);
    switch (result.status) {
    case CWallet::ScanResult::SUCCESS:
        break;
    case CWallet::ScanResult::FAILURE:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan failed. Potentially corrupted data files.");
    case CWallet::ScanResult::USER_ABORT:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        // no default case, so the compiler can warn about missing cases
    }
    UniValue response(UniValue::VOBJ);
    response.pushKV("start_height", start_height);
    response.pushKV("stop_height", result.last_scanned_height ? *result.last_scanned_height : UniValue());
    return response;
}

class DescribeWalletAddressVisitor : public boost::static_visitor<UniValue>
{
public:
    const SigningProvider * const provider;

    void ProcessSubScript(const CScript& subscript, UniValue& obj) const
    {
        // Always present: script type and redeemscript
        std::vector<std::vector<unsigned char>> solutions_data;
        txnouttype which_type = Solver(subscript, solutions_data);
        obj.pushKV("script", GetTxnOutputType(which_type));
        obj.pushKV("hex", HexStr(subscript.begin(), subscript.end()));

        CTxDestination embedded;
        if (ExtractDestination(subscript, embedded)) {
            // Only when the script corresponds to an address.
            UniValue subobj(UniValue::VOBJ);
            UniValue detail = DescribeAddress(embedded);
            subobj.pushKVs(detail);
            UniValue wallet_detail = boost::apply_visitor(*this, embedded);
            subobj.pushKVs(wallet_detail);
            subobj.pushKV("address", EncodeDestination(embedded));
            subobj.pushKV("scriptPubKey", HexStr(subscript.begin(), subscript.end()));
            // Always report the pubkey at the top level, so that `getnewaddress()['pubkey']` always works.
            if (subobj.exists("pubkey")) obj.pushKV("pubkey", subobj["pubkey"]);
            obj.pushKV("embedded", std::move(subobj));
        } else if (which_type == TX_MULTISIG) {
            // Also report some information on multisig scripts (which do not have a corresponding address).
            // TODO: abstract out the common functionality between this logic and ExtractDestinations.
            obj.pushKV("sigsrequired", solutions_data[0][0]);
            UniValue pubkeys(UniValue::VARR);
            for (size_t i = 1; i < solutions_data.size() - 1; ++i) {
                CPubKey key(solutions_data[i].begin(), solutions_data[i].end());
                pubkeys.push_back(HexStr(key.begin(), key.end()));
            }
            obj.pushKV("pubkeys", std::move(pubkeys));
        }
    }

    explicit DescribeWalletAddressVisitor(const SigningProvider* _provider) : provider(_provider) {}

    UniValue operator()(const CNoDestination& dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const PKHash& pkhash) const
    {
        CKeyID keyID(pkhash);
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        if (provider && provider->GetPubKey(keyID, vchPubKey)) {
            obj.pushKV("pubkey", HexStr(vchPubKey));
            obj.pushKV("iscompressed", vchPubKey.IsCompressed());
        }
        return obj;
    }

    UniValue operator()(const ScriptHash& scripthash) const
    {
        CScriptID scriptID(scripthash);
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        if (provider && provider->GetCScript(scriptID, subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessV0KeyHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey pubkey;
        if (provider && provider->GetPubKey(CKeyID(id), pubkey)) {
            obj.pushKV("pubkey", HexStr(pubkey));
        }
        return obj;
    }

    UniValue operator()(const WitnessV0ScriptHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        CRIPEMD160 hasher;
        uint160 hash;
        hasher.Write(id.begin(), 32).Finalize(hash.begin());
        if (provider && provider->GetCScript(CScriptID(hash), subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessUnknown& id) const { return UniValue(UniValue::VOBJ); }
};

static UniValue DescribeWalletAddress(CWallet* pwallet, const CTxDestination& dest)
{
    UniValue ret(UniValue::VOBJ);
    UniValue detail = DescribeAddress(dest);
    CScript script = GetScriptForDestination(dest);
    std::unique_ptr<SigningProvider> provider = nullptr;
    if (pwallet) {
        provider = pwallet->GetSigningProvider(script);
    }
    ret.pushKVs(detail);
    ret.pushKVs(boost::apply_visitor(DescribeWalletAddressVisitor(provider.get()), dest));
    return ret;
}

/** Convert CAddressBookData to JSON record.  */
static UniValue AddressBookDataToJSON(const CAddressBookData& data, const bool verbose)
{
    UniValue ret(UniValue::VOBJ);
    if (verbose) {
        ret.pushKV("name", data.name);
    }
    ret.pushKV("purpose", data.purpose);
    return ret;
}

UniValue getaddressinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    const std::string example_address = "\"bc1q09vm5lfy0j5reeulh4x5752q25uqqvz34hufdl\"";

            RPCHelpMan{"getaddressinfo",
                "\nReturn information about the given bitcoin address.\n"
                "Some of the information will only be present if the address is in the active wallet.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The bitcoin address for which to get information."},
                },
                RPCResult{
            "{\n"
            "  \"address\" : \"address\",              (string) The bitcoin address validated.\n"
            "  \"scriptPubKey\" : \"hex\",             (string) The hex-encoded scriptPubKey generated by the address.\n"
            "  \"ismine\" : true|false,              (boolean) If the address is yours.\n"
            "  \"iswatchonly\" : true|false,         (boolean) If the address is watchonly.\n"
            "  \"solvable\" : true|false,            (boolean) If we know how to spend coins sent to this address, ignoring the possible lack of private keys.\n"
            "  \"desc\" : \"desc\",                    (string, optional) A descriptor for spending coins sent to this address (only when solvable).\n"
            "  \"isscript\" : true|false,            (boolean) If the key is a script.\n"
            "  \"ischange\" : true|false,            (boolean) If the address was used for change output.\n"
            "  \"iswitness\" : true|false,           (boolean) If the address is a witness address.\n"
            "  \"witness_version\" : version         (numeric, optional) The version number of the witness program.\n"
            "  \"witness_program\" : \"hex\"           (string, optional) The hex value of the witness program.\n"
            "  \"script\" : \"type\"                   (string, optional) The output script type. Only if isscript is true and the redeemscript is known. Possible\n"
            "                                                         types: nonstandard, pubkey, pubkeyhash, scripthash, multisig, nulldata, witness_v0_keyhash,\n"
            "                                                         witness_v0_scripthash, witness_unknown.\n"
            "  \"hex\" : \"hex\",                      (string, optional) The redeemscript for the p2sh address.\n"
            "  \"pubkeys\"                           (array, optional) Array of pubkeys associated with the known redeemscript (only if script is multisig).\n"
            "    [\n"
            "      \"pubkey\" (string)\n"
            "      ,...\n"
            "    ]\n"
            "  \"sigsrequired\" : xxxxx              (numeric, optional) The number of signatures required to spend multisig output (only if script is multisig).\n"
            "  \"pubkey\" : \"publickeyhex\",          (string, optional) The hex value of the raw public key for single-key addresses (possibly embedded in P2SH or P2WSH).\n"
            "  \"embedded\" : {...},                 (object, optional) Information about the address embedded in P2SH or P2WSH, if relevant and known. Includes all\n"
            "                                                         getaddressinfo output fields for the embedded address, excluding metadata (timestamp, hdkeypath,\n"
            "                                                         hdseedid) and relation to the wallet (ismine, iswatchonly).\n"
            "  \"iscompressed\" : true|false,        (boolean, optional) If the pubkey is compressed.\n"
            "  \"label\" :  \"label\"                  (string) The label associated with the address. Defaults to \"\". Equivalent to the label name in the labels array below.\n"
            "  \"timestamp\" : timestamp,            (number, optional) The creation time of the key, if available, expressed in " + UNIX_EPOCH_TIME + ".\n"
            "  \"hdkeypath\" : \"keypath\"             (string, optional) The HD keypath, if the key is HD and available.\n"
            "  \"hdseedid\" : \"<hash160>\"            (string, optional) The Hash160 of the HD seed.\n"
            "  \"hdmasterfingerprint\" : \"<hash160>\" (string, optional) The fingerprint of the master key.\n"
            "  \"labels\"                            (json object) An array of labels associated with the address. Currently limited to one label but returned\n"
            "                                               as an array to keep the API stable if multiple labels are enabled in the future.\n"
            "    [\n"
            "      \"label name\" (string) The label name. Defaults to \"\". Equivalent to the label field above.\n\n"
            "      DEPRECATED, will be removed in 0.21. To re-enable, launch bitcoind with `-deprecatedrpc=labelspurpose`:\n"
            "      { (json object of label data)\n"
            "        \"name\" : \"label name\" (string) The label name. Defaults to \"\". Equivalent to the label field above.\n"
            "        \"purpose\" : \"purpose\" (string) The purpose of the associated address (send or receive).\n"
            "      }\n"
            "    ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getaddressinfo", example_address) +
                    HelpExampleRpc("getaddressinfo", example_address)
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    UniValue ret(UniValue::VOBJ);
    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    // Make sure the destination is valid
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::string currentAddress = EncodeDestination(dest);
    ret.pushKV("address", currentAddress);

    CScript scriptPubKey = GetScriptForDestination(dest);
    ret.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));

    std::unique_ptr<SigningProvider> provider = pwallet->GetSigningProvider(scriptPubKey);

    isminetype mine = pwallet->IsMine(dest);
    ret.pushKV("ismine", bool(mine & ISMINE_SPENDABLE));

    bool solvable = provider && IsSolvable(*provider, scriptPubKey);
    ret.pushKV("solvable", solvable);

    if (solvable) {
       ret.pushKV("desc", InferDescriptor(scriptPubKey, *provider)->ToString());
    }

    ret.pushKV("iswatchonly", bool(mine & ISMINE_WATCH_ONLY));

    UniValue detail = DescribeWalletAddress(pwallet, dest);
    ret.pushKVs(detail);

    // Return label field if existing. Currently only one label can be
    // associated with an address, so the label should be equivalent to the
    // value of the name key/value pair in the labels array below.
    if (pwallet->mapAddressBook.count(dest)) {
        ret.pushKV("label", pwallet->mapAddressBook[dest].name);
    }

    ret.pushKV("ischange", pwallet->IsChange(scriptPubKey));

    ScriptPubKeyMan* spk_man = pwallet->GetScriptPubKeyMan(scriptPubKey);
    if (spk_man) {
        if (const CKeyMetadata* meta = spk_man->GetMetadata(dest)) {
            ret.pushKV("timestamp", meta->nCreateTime);
            if (meta->has_key_origin) {
                ret.pushKV("hdkeypath", WriteHDKeypath(meta->key_origin.path));
                ret.pushKV("hdseedid", meta->hd_seed_id.GetHex());
                ret.pushKV("hdmasterfingerprint", HexStr(meta->key_origin.fingerprint, meta->key_origin.fingerprint + 4));
            }
        }
    }

    // Return a `labels` array containing the label associated with the address,
    // equivalent to the `label` field above. Currently only one label can be
    // associated with an address, but we return an array so the API remains
    // stable if we allow multiple labels to be associated with an address in
    // the future.
    //
    // DEPRECATED: The previous behavior of returning an array containing a JSON
    // object of `name` and `purpose` key/value pairs has been deprecated.
    UniValue labels(UniValue::VARR);
    std::map<CTxDestination, CAddressBookData>::iterator mi = pwallet->mapAddressBook.find(dest);
    if (mi != pwallet->mapAddressBook.end()) {
        if (pwallet->chain().rpcEnableDeprecated("labelspurpose")) {
            labels.push_back(AddressBookDataToJSON(mi->second, true));
        } else {
            labels.push_back(mi->second.name);
        }
    }
    ret.pushKV("labels", std::move(labels));

    return ret;
}

static UniValue getaddressesbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"getaddressesbylabel",
                "\nReturns the list of addresses assigned the specified label.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label."},
                },
                RPCResult{
            "{ (json object with addresses as keys)\n"
            "  \"address\": { (json object with information about address)\n"
            "    \"purpose\": \"string\" (string)  Purpose of address (\"send\" for sending address, \"receive\" for receiving address)\n"
            "  },...\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("getaddressesbylabel", "\"tabby\"")
            + HelpExampleRpc("getaddressesbylabel", "\"tabby\"")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    std::string label = LabelFromValue(request.params[0]);

    // Find all addresses that have the given label
    UniValue ret(UniValue::VOBJ);
    std::set<std::string> addresses;
    for (const std::pair<const CTxDestination, CAddressBookData>& item : pwallet->mapAddressBook) {
        if (item.second.name == label) {
            std::string address = EncodeDestination(item.first);
            // CWallet::mapAddressBook is not expected to contain duplicate
            // address strings, but build a separate set as a precaution just in
            // case it does.
            bool unique = addresses.emplace(address).second;
            CHECK_NONFATAL(unique);
            // UniValue::pushKV checks if the key exists in O(N)
            // and since duplicate addresses are unexpected (checked with
            // std::set in O(log(N))), UniValue::__pushKV is used instead,
            // which currently is O(1).
            ret.__pushKV(address, AddressBookDataToJSON(item.second, false));
        }
    }

    if (ret.empty()) {
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, std::string("No addresses with label " + label));
    }

    return ret;
}

static UniValue listlabels(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"listlabels",
                "\nReturns the list of all labels, or labels that are assigned to addresses with a specific purpose.\n",
                {
                    {"purpose", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Address purpose to list labels for ('send','receive'). An empty string is the same as not providing this argument."},
                },
                RPCResult{
            "[               (json array of string)\n"
            "  \"label\",      (string) Label name\n"
            "  ...\n"
            "]\n"
                },
                RPCExamples{
            "\nList all labels\n"
            + HelpExampleCli("listlabels", "") +
            "\nList labels that have receiving addresses\n"
            + HelpExampleCli("listlabels", "receive") +
            "\nList labels that have sending addresses\n"
            + HelpExampleCli("listlabels", "send") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("listlabels", "receive")
                },
            }.Check(request);

    LOCK(pwallet->cs_wallet);

    std::string purpose;
    if (!request.params[0].isNull()) {
        purpose = request.params[0].get_str();
    }

    // Add to a set to sort by label name, then insert into Univalue array
    std::set<std::string> label_set;
    for (const std::pair<const CTxDestination, CAddressBookData>& entry : pwallet->mapAddressBook) {
        if (purpose.empty() || entry.second.purpose == purpose) {
            label_set.insert(entry.second.name);
        }
    }

    UniValue ret(UniValue::VARR);
    for (const std::string& name : label_set) {
        ret.push_back(name);
    }

    return ret;
}

UniValue sethdseed(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"sethdseed",
                "\nSet or generate a new HD wallet seed. Non-HD wallets will not be upgraded to being a HD wallet. Wallets that are already\n"
                "HD will have a new HD seed set so that new keys added to the keypool will be derived from this new seed.\n"
                "\nNote that you will need to MAKE A NEW BACKUP of your wallet after setting the HD wallet seed." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newkeypool", RPCArg::Type::BOOL, /* default */ "true", "Whether to flush old unused addresses, including change addresses, from the keypool and regenerate it.\n"
            "                             If true, the next address from getnewaddress and change address from getrawchangeaddress will be from this new seed.\n"
            "                             If false, addresses (including change addresses if the wallet already had HD Chain Split enabled) from the existing\n"
            "                             keypool will be used until it has been depleted."},
                    {"seed", RPCArg::Type::STR, /* default */ "random seed", "The WIF private key to use as the new HD seed.\n"
            "                             The seed value can be retrieved using the dumpwallet command. It is the private key marked hdseed=1"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("sethdseed", "")
            + HelpExampleCli("sethdseed", "false")
            + HelpExampleCli("sethdseed", "true \"wifkey\"")
            + HelpExampleRpc("sethdseed", "true, \"wifkey\"")
                },
            }.Check(request);

    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*pwallet, true);

    if (pwallet->chain().isInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot set a new HD seed while still in Initial Block Download");
    }

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed to a wallet with private keys disabled");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK2(pwallet->cs_wallet, spk_man.cs_KeyStore);

    // Do not do anything to non-HD wallets
    if (!pwallet->CanSupportFeature(FEATURE_HD)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed on a non-HD wallet. Start with -upgradewallet in order to upgrade a non-HD wallet to HD");
    }

    EnsureWalletIsUnlocked(pwallet);

    bool flush_key_pool = true;
    if (!request.params[0].isNull()) {
        flush_key_pool = request.params[0].get_bool();
    }

    CPubKey master_pub_key;
    if (request.params[1].isNull()) {
        master_pub_key = spk_man.GenerateNewSeed();
    } else {
        CKey key = DecodeSecret(request.params[1].get_str());
        if (!key.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
        }

        if (HaveKey(spk_man, key)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key (either as an HD seed or as a loose private key)");
        }

        master_pub_key = spk_man.DeriveNewSeed(key);
    }

    spk_man.SetHDSeed(master_pub_key);
    if (flush_key_pool) spk_man.NewKeyPool();

    return NullUniValue;
}

UniValue walletprocesspsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletprocesspsbt",
                "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
                "that we can sign for." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, /* default */ "true", "Also sign the transaction when updating"},
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type to sign with if not specified by the PSBT. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
            "{\n"
            "  \"psbt\" : \"value\",          (string) The base64-encoded partially signed transaction\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  ]\n"
            "}\n"
                },
                RPCExamples{
                    HelpExampleCli("walletprocesspsbt", "\"psbt\"")
                },
            }.Check(request);

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL, UniValue::VSTR});

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Fill transaction with our data and also sign
    bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
    bool bip32derivs = request.params[3].isNull() ? false : request.params[3].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, nHashType, sign, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    UniValue result(UniValue::VOBJ);
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);

    return result;
}

UniValue walletcreatefundedpsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

            RPCHelpMan{"walletcreatefundedpsbt",
                "\nCreates and funds a transaction in the Partially Signed Transaction format. Inputs will be added if supplied inputs are not enough\n"
                "Implements the Creator and Updater roles.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "A json array of json objects",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "a json array with outputs (key-value pairs), where none of the keys are duplicated.\n"
                            "That is, each address can only appear once and there can only be one 'data' object.\n"
                            "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                            "                             accepted as second parameter.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the bitcoin address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                },
                                },
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"changeAddress", RPCArg::Type::STR_HEX, /* default */ "pool address", "The bitcoin address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, /* default */ "true for watch-only wallets, otherwise false", "Also select inputs which are watch only"},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, /* default */ "empty array", "A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "Fallback to wallet's confirmation target", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                            "{\n"
                            "  \"psbt\": \"value\",        (string)  The resulting raw transaction (base64-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
                                },
                            }.Check(request);

    RPCTypeCheck(request.params, {
        UniValue::VARR,
        UniValueType(), // ARR or OBJ, checked later
        UniValue::VNUM,
        UniValue::VOBJ,
        UniValue::VBOOL
        }, true
    );

    CAmount fee;
    int change_position;
    bool rbf = pwallet->m_signal_rbf;
    const UniValue &replaceable_arg = request.params[3]["replaceable"];
    if (!replaceable_arg.isNull()) {
        RPCTypeCheckArgument(replaceable_arg, UniValue::VBOOL);
        rbf = replaceable_arg.isTrue();
    }
    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], rbf);
    FundTransaction(pwallet, rawTx, fee, change_position, request.params[3]);

    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[4].isNull() ? false : request.params[4].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, 1, false, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);
    return result;
}

UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue importprivkey(const JSONRPCRequest& request);
UniValue importaddress(const JSONRPCRequest& request);
UniValue importpubkey(const JSONRPCRequest& request);
UniValue dumpwallet(const JSONRPCRequest& request);
UniValue importwallet(const JSONRPCRequest& request);
UniValue importprunedfunds(const JSONRPCRequest& request);
UniValue removeprunedfunds(const JSONRPCRequest& request);
UniValue importmulti(const JSONRPCRequest& request);

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                                actor (function)                argNames
    //  --------------------- ------------------------          -----------------------         ----------
    { "rawtransactions",    "fundrawtransaction",               &fundrawtransaction,            {"hexstring","options","iswitness"} },
    { "wallet",             "abandontransaction",               &abandontransaction,            {"txid"} },
    { "wallet",             "abortrescan",                      &abortrescan,                   {} },
    { "wallet",             "addmultisigaddress",               &addmultisigaddress,            {"nrequired","keys","label","address_type"} },
    { "wallet",             "backupwallet",                     &backupwallet,                  {"destination"} },
    { "wallet",             "bumpfee",                          &bumpfee,                       {"txid", "options"} },
    { "wallet",             "createwallet",                     &createwallet,                  {"wallet_name", "disable_private_keys", "blank", "passphrase", "avoid_reuse"} },
    { "wallet",             "create_ctv_vault",                 &create_ctv_vault,              {"amount", "steps", "step_period", "maturity", "hot_address", "cold_address"} },
    { "wallet",             "dumpprivkey",                      &dumpprivkey,                   {"address"}  },
    { "wallet",             "dumpwallet",                       &dumpwallet,                    {"filename"} },
    { "wallet",             "encryptwallet",                    &encryptwallet,                 {"passphrase"} },
    { "wallet",             "getaddressesbylabel",              &getaddressesbylabel,           {"label"} },
    { "wallet",             "getaddressinfo",                   &getaddressinfo,                {"address"} },
    { "wallet",             "getbalance",                       &getbalance,                    {"dummy","minconf","include_watchonly","avoid_reuse"} },
    { "wallet",             "getnewaddress",                    &getnewaddress,                 {"label","address_type"} },
    { "wallet",             "getrawchangeaddress",              &getrawchangeaddress,           {"address_type"} },
    { "wallet",             "getreceivedbyaddress",             &getreceivedbyaddress,          {"address","minconf"} },
    { "wallet",             "getreceivedbylabel",               &getreceivedbylabel,            {"label","minconf"} },
    { "wallet",             "gettransaction",                   &gettransaction,                {"txid","include_watchonly","verbose"} },
    { "wallet",             "getunconfirmedbalance",            &getunconfirmedbalance,         {} },
    { "wallet",             "getbalances",                      &getbalances,                   {} },
    { "wallet",             "getwalletinfo",                    &getwalletinfo,                 {} },
    { "wallet",             "importaddress",                    &importaddress,                 {"address","label","rescan","p2sh"} },
    { "wallet",             "importmulti",                      &importmulti,                   {"requests","options"} },
    { "wallet",             "importprivkey",                    &importprivkey,                 {"privkey","label","rescan"} },
    { "wallet",             "importprunedfunds",                &importprunedfunds,             {"rawtransaction","txoutproof"} },
    { "wallet",             "importpubkey",                     &importpubkey,                  {"pubkey","label","rescan"} },
    { "wallet",             "importwallet",                     &importwallet,                  {"filename"} },
    { "wallet",             "keypoolrefill",                    &keypoolrefill,                 {"newsize"} },
    { "wallet",             "listaddressgroupings",             &listaddressgroupings,          {} },
    { "wallet",             "listlabels",                       &listlabels,                    {"purpose"} },
    { "wallet",             "listlockunspent",                  &listlockunspent,               {} },
    { "wallet",             "listreceivedbyaddress",            &listreceivedbyaddress,         {"minconf","include_empty","include_watchonly","address_filter"} },
    { "wallet",             "listreceivedbylabel",              &listreceivedbylabel,           {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listsinceblock",                   &listsinceblock,                {"blockhash","target_confirmations","include_watchonly","include_removed"} },
    { "wallet",             "listtransactions",                 &listtransactions,              {"label|dummy","count","skip","include_watchonly"} },
    { "wallet",             "listunspent",                      &listunspent,                   {"minconf","maxconf","addresses","include_unsafe","query_options"} },
    { "wallet",             "listwalletdir",                    &listwalletdir,                 {} },
    { "wallet",             "listwallets",                      &listwallets,                   {} },
    { "wallet",             "loadwallet",                       &loadwallet,                    {"filename"} },
    { "wallet",             "lockunspent",                      &lockunspent,                   {"unlock","transactions"} },
    { "wallet",             "removeprunedfunds",                &removeprunedfunds,             {"txid"} },
    { "wallet",             "rescanblockchain",                 &rescanblockchain,              {"start_height", "stop_height"} },
    { "wallet",             "sendmany",                         &sendmany,                      {"dummy","amounts","minconf","comment","subtractfeefrom","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "sendmanycompacted",                &sendmanycompacted,             {"amounts","radix","gas","node_fees","comment","root_replaceable","root_conf_target","root_estimate","pairing_mode","probabilities"} },
    { "wallet",             "sendtoaddress",                    &sendtoaddress,                 {"address","amount","comment","comment_to","subtractfeefromamount","replaceable","conf_target","estimate_mode","avoid_reuse"} },
    { "wallet",             "sethdseed",                        &sethdseed,                     {"newkeypool","seed"} },
    { "wallet",             "setlabel",                         &setlabel,                      {"address","label"} },
    { "wallet",             "settxfee",                         &settxfee,                      {"amount"} },
    { "wallet",             "setwalletflag",                    &setwalletflag,                 {"flag","value"} },
    { "wallet",             "signmessage",                      &signmessage,                   {"address","message"} },
    { "wallet",             "signrawtransactionwithwallet",     &signrawtransactionwithwallet,  {"hexstring","prevtxs","sighashtype"} },
    { "wallet",             "unloadwallet",                     &unloadwallet,                  {"wallet_name"} },
    { "wallet",             "walletcreatefundedpsbt",           &walletcreatefundedpsbt,        {"inputs","outputs","locktime","options","bip32derivs"} },
    { "wallet",             "walletlock",                       &walletlock,                    {} },
    { "wallet",             "walletpassphrase",                 &walletpassphrase,              {"passphrase","timeout"} },
    { "wallet",             "walletpassphrasechange",           &walletpassphrasechange,        {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletprocesspsbt",                &walletprocesspsbt,             {"psbt","sign","sighashtype","bip32derivs"} },
};
// clang-format on

void RegisterWalletRPCCommands(interfaces::Chain& chain, std::vector<std::unique_ptr<interfaces::Handler>>& handlers)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        handlers.emplace_back(chain.handleRpc(commands[vcidx]));
}

interfaces::Chain* g_rpc_chain = nullptr;
