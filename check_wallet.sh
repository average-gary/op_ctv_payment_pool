#!/bin/bash

# Set the bitcoin-cli path
BITCOIN_CLI="/Users/garykrause/repos/bitcoin/build/bin/bitcoin-cli -testnet4"
WALLET="testnet4_wallet"

echo -e "\n=== Wallet: $WALLET ==="

# Load the wallet
$BITCOIN_CLI -rpcwallet=$WALLET loadwallet $WALLET > /dev/null 2>&1

# Get balance
BALANCE=$($BITCOIN_CLI -rpcwallet=$WALLET getbalance)
echo "Balance: $BALANCE BTC"

# Get unconfirmed balance
UNCONFIRMED=$($BITCOIN_CLI -rpcwallet=$WALLET getunconfirmedbalance)
if [ "$UNCONFIRMED" != "0.00000000" ]; then
    echo "Unconfirmed balance: $UNCONFIRMED BTC"
fi

# Get unconfirmed transactions
echo "Unconfirmed transactions:"
$BITCOIN_CLI -rpcwallet=$WALLET listunspent 0 0 | grep -A 2 "confirmations\": 0"

# Get mempool transactions
echo "Mempool transactions:"
$BITCOIN_CLI -rpcwallet=$WALLET listunspent 0 0 | grep -A 2 "confirmations\": 0"

echo -e "\n=== Mempool Info ==="
$BITCOIN_CLI getmempoolinfo 