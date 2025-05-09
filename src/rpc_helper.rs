use std::collections::HashMap;

use anyhow::Result;
use bitcoin::{
    absolute, consensus::encode::serialize_hex, transaction, Address, Amount, OutPoint, Sequence,
    Transaction, TxIn, TxOut, Txid,
};
use bitcoincore_rpc::{
    json::{self, GetTransactionResultDetail},
    jsonrpc::serde_json,
    Client, RpcApi,
};
use serde_json::json;
use tracing::info;

use crate::{
    config::{NetworkConfig, DEFAULT_FEE_RATE, DUST_AMOUNT, INIT_WALLET_AMOUNT_FEE, TX_VERSION},
    AMOUNT_PER_USER, POOL_USERS,
};

pub fn send_funding_transaction(rpc: &Client, config: &NetworkConfig, fee_amount: Amount) -> Txid {
    info!("Creating funding transaction:");
    info!("  Amount per user: {}", AMOUNT_PER_USER);
    info!("  Number of users: {}", POOL_USERS);
    info!("  Total amount: {}", AMOUNT_PER_USER * POOL_USERS.try_into().unwrap());
    
    let change_address = rpc.get_raw_change_address(None).unwrap();
    info!("  Change address: {:?}", change_address);

    let unspent = rpc.list_unspent(Some(1), None, None, None, None).unwrap();
    info!("  Number of unspent outputs: {}", unspent.len());
    
    let mut inputs = Vec::new();
    let mut total_input = Amount::ZERO;
    
    for utxo in unspent {
        info!("  Using UTXO:");
        info!("    TXID: {}", utxo.txid);
        info!("    Vout: {}", utxo.vout);
        info!("    Amount: {}", utxo.amount);
        
        inputs.push(TxIn {
            previous_output: OutPoint {
                txid: utxo.txid,
                vout: utxo.vout,
            },
            sequence: Sequence::ENABLE_RBF_NO_LOCKTIME,
            ..Default::default()
        });
        
        total_input += utxo.amount;
    }
    
    info!("  Total input amount: {}", total_input);
    
    let outputs = vec![
        TxOut {
            value: AMOUNT_PER_USER * POOL_USERS.try_into().unwrap() - fee_amount,
            script_pubkey: change_address.assume_checked().script_pubkey(),
        },
    ];
    
    let unsigned_tx = Transaction {
        version: transaction::Version(TX_VERSION),
        lock_time: absolute::LockTime::ZERO,
        input: inputs,
        output: outputs,
    };
    
    let serialized_tx = serialize_hex(&unsigned_tx);
    info!("  Serialized transaction: {:?}", serialized_tx);
    
    let signed_tx = rpc
        .sign_raw_transaction_with_wallet(serialized_tx, None, None)
        .unwrap();
    info!("  Signed transaction: {:?}", signed_tx.hex);
    
    let txid = rpc.send_raw_transaction(&signed_tx.hex).unwrap();
    info!("  Transaction ID: {}", txid);
    
    txid
}

pub fn simulate_psbt_signing(
    rpc: &Client,
    previous_txid: Txid,
    pool_address: &Address,
    fee_amount: Amount,
) -> Result<Txid> {
    info!("Simulating PSBT signing:");
    info!("  Previous transaction ID: {}", previous_txid);
    info!("  Pool address: {:?}", pool_address);
    
    let previous_tx: Transaction = rpc.get_raw_transaction(&previous_txid, None).unwrap();
    info!("  Previous transaction outputs:");
    for (i, output) in previous_tx.output.iter().enumerate() {
        info!("    Output {}: Amount {}", i, output.value);
    }
    
    let vout = previous_tx
        .output
        .iter()
        .position(|vout| vout.value == AMOUNT_PER_USER * POOL_USERS.try_into().unwrap() - fee_amount)
        .unwrap() as u32;
    info!("  Using vout: {}", vout);
    
    let inputs = vec![TxIn {
        previous_output: OutPoint {
            txid: previous_txid,
            vout,
        },
        sequence: Sequence::ENABLE_RBF_NO_LOCKTIME,
        ..Default::default()
    }];
    
    let outputs = vec![TxOut {
        value: AMOUNT_PER_USER * POOL_USERS.try_into().unwrap(),
        script_pubkey: pool_address.script_pubkey(),
    }];
    
    let unsigned_tx = Transaction {
        version: transaction::Version(TX_VERSION),
        lock_time: absolute::LockTime::ZERO,
        input: inputs,
        output: outputs,
    };
    
    let serialized_tx = serialize_hex(&unsigned_tx);
    info!("  Serialized transaction: {:?}", serialized_tx);
    
    let signed_tx = rpc
        .sign_raw_transaction_with_wallet(serialized_tx, None, None)
        .unwrap();
    info!("  Signed transaction: {:?}", signed_tx.hex);
    
    let txid = rpc.send_raw_transaction(&signed_tx.hex)?;
    info!("  Transaction ID: {}", txid);
    
    Ok(txid)
}

pub fn get_vouts_from_init_tx(rpc: &Client, txid: &Txid) -> Vec<GetTransactionResultDetail> {
    let tx = rpc.get_transaction(txid, None).unwrap();
    let tx_details = tx.details;

    let matched_vouts: Vec<GetTransactionResultDetail> = tx_details
        .iter()
        .filter(|vout| {
            vout.amount
                == (AMOUNT_PER_USER + INIT_WALLET_AMOUNT_FEE)
                    .to_signed()
                    .unwrap()
        })
        .cloned()
        .collect();

    matched_vouts
}
