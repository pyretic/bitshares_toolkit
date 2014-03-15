#include <bts/wallet/wallet.hpp>
#include <bts/wallet/extended_address.hpp>
#include <bts/blockchain/chain_database.hpp>
#include <bts/blockchain/pts_address.hpp>
//#include <bts/bitcoin_wallet.hpp>
#include <unordered_map>
#include <map>
#include <fc/filesystem.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/crypto/aes.hpp>
#include <sstream>

#include <iostream>
#include <iomanip>

namespace bts { namespace wallet {



   /** 
    * this is the data that is stored on disk in an encrypted form protected by
    * the wallet password.
    */
   struct wallet_data 
   {
       uint32_t                                                 version;
       uint32_t                                                 last_used_key;
       uint32_t                                                 last_scanned_block_num;
       std::unordered_map<address,std::string>                  recv_addresses;
       std::unordered_map<pts_address,address>                  recv_pts_addresses;
       std::unordered_map<address,std::string>                  send_addresses;

       //std::vector<fc::ecc::private_key>                 keys;
       // an aes encrypted std::unordered_map<address,fc::ecc::private_key>
       std::vector<char>                                        encrypted_keys;
       std::vector<char>                                        encrypted_base_key;

       std::unordered_map<address,fc::ecc::private_key>    get_keys( const std::string& password )
       { try {
          ilog( "get_keys with password '${pass}'", ("pass",password) );
          std::unordered_map<address, fc::ecc::private_key> keys;
          if( encrypted_keys.size() == 0 ) return keys;
          //ilog( "encrypted keys.size: ${s}", ("s",encrypted_keys.size() ) );

          auto plain_txt = fc::aes_decrypt( fc::sha512::hash( password.c_str(), password.size() ), encrypted_keys );
          //ilog( "plain_txt '${p}' size ${s}", ("p",plain_txt)("s",plain_txt.size()) );
          fc::datastream<const char*> ds(plain_txt.data(),plain_txt.size());
          fc::raw::unpack( ds, keys );
          return keys;
       } FC_RETHROW_EXCEPTIONS( warn, "" ) }

       extended_private_key                                     get_base_key( const std::string& password )
       {
          ilog( "get_base_key  with password '${pass}'  encrypted_base_key ${ebk}", ("pass",password)("ebk",encrypted_base_key.size()) );
          extended_private_key base_key;
          if( encrypted_base_key.size() == 0 ) return base_key;

          auto plain_txt = fc::aes_decrypt( fc::sha512::hash( password.c_str(), password.size() ), encrypted_base_key );
          return fc::raw::unpack<extended_private_key>( plain_txt );
       }

       void set_keys( const std::unordered_map<address,fc::ecc::private_key>& k, const std::string& password )
       {
          auto plain_txt = fc::raw::pack( k );
          //ilog( "new_password '${p}'  plaint_txt '${pt}' size ${s}", ("p",password)("pt",plain_txt)("s",plain_txt.size()) );
          encrypted_keys = fc::aes_encrypt( fc::sha512::hash( password.c_str(), password.size() ), plain_txt );
          FC_ASSERT( k == get_keys(password) );
       }

       void change_password( const std::string& old_password, const std::string& new_password )
       {
          set_keys( get_keys( old_password ), new_password );
          set_base_key( get_base_key( old_password ), new_password );
       }

       void set_base_key( const extended_private_key& bk, const std::string& new_password )
       {
          auto plain_txt = fc::raw::pack( bk );
          //ilog( "new_password '${p}'  plaint_txt ${pt}", ("p",new_password)("pt",plain_txt) );
          encrypted_base_key = fc::aes_encrypt( fc::sha512::hash( new_password.c_str(), new_password.size() ), plain_txt );
          auto check = fc::aes_decrypt( fc::sha512::hash( new_password.c_str(), new_password.size() ), encrypted_base_key );
          FC_ASSERT( check == plain_txt );
       }

       // 
       std::unordered_map<transaction_id_type, transaction_state> transactions;
   };
} } // bts::wallet


FC_REFLECT( bts::wallet::wallet_data, 
            (version)
            (last_used_key)
            (last_scanned_block_num)
            (recv_addresses)
            (recv_pts_addresses)
            (send_addresses)
            (encrypted_base_key)
            (encrypted_keys)
            (transactions) 
            )

namespace bts { namespace wallet {
  
   output_index::operator std::string()const
   {
      std::stringstream ss;
      ss<<block_idx<<"."<<trx_idx<<"."<<output_idx;
      return ss.str();
   }

   namespace detail 
   {
      class wallet_impl
      {
          public:
              wallet_impl():_stake(0),_exception_on_open(false){}
              std::string _wallet_base_password; // used for saving/loading the wallet
              std::string _wallet_key_password;  // used to access private keys

              fc::path                                                   _wallet_dat;
              wallet_data                                                _data;
              asset                                                      _current_fee_rate;
              uint64_t                                                   _stake;
              bool                                                       _exception_on_open;

              std::map<output_index, output_reference>                   _output_index_to_ref;
              std::unordered_map<output_reference, output_index>         _output_ref_to_index;

              // keep sorted so we spend oldest first to maximize CDD
              std::map<output_index, trx_output>                         _unspent_outputs;
              std::map<output_index, trx_output>                         _spent_outputs;

              // maps address to private key index
              std::unordered_map<address,fc::ecc::private_key>      _my_keys;
              std::unordered_map<transaction_id_type,signed_transaction> _id_to_signed_transaction;

              asset get_fee_rate()
              {
                  return _current_fee_rate;
              }

              asset get_balance( asset::type balance_type )
              {
                   asset total_bal( static_cast<uint64_t>(0ull), balance_type);
                   std::vector<trx_input> inputs;
                   for( auto itr = _unspent_outputs.begin(); itr != _unspent_outputs.end(); ++itr )
                   {
                      //ilog( "unspent outputs ${o}", ("o",*itr) );
                       if( itr->second.claim_func == claim_by_signature && itr->second.amount.unit == balance_type )
                       {
                           total_bal += itr->second.amount; // TODO: apply interest earned 
                       }
                       if( itr->second.claim_func == claim_by_pts && itr->second.amount.unit == balance_type )
                       {
                           total_bal += itr->second.amount; // TODO: apply interest earned 
                       }
                   }
                   return total_bal;
              }

              std::vector<trx_input> collect_coindays( uint64_t request_cdd, asset& total_in, 
                                                       std::unordered_set<address>& req_sigs, uint64_t& provided_cdd )
              {
                   FC_ASSERT( _data.last_scanned_block_num > 0 );
                   provided_cdd = 0;
                   std::vector<trx_input> inputs;
                   for( auto itr = _unspent_outputs.begin(); itr != _unspent_outputs.end(); ++itr )
                   {
                       ilog( "unspent outputs ${o}", ("o",*itr) );
                       if( itr->second.claim_func == claim_by_signature && itr->second.amount.unit == 0 )
                       {
                           inputs.push_back( trx_input( _output_index_to_ref[itr->first] ) );
                           total_in += itr->second.amount;
                           auto cdd = itr->second.amount.get_rounded_amount() * (_data.last_scanned_block_num - itr->first.block_idx);
                           if( cdd > 0 ) 
                           {
                              provided_cdd += cdd;
                              req_sigs.insert( itr->second.as<claim_by_signature_output>().owner );
                             // ilog( "total in ${in}  min ${min}", ( "in",total_in)("min",min_amnt) );
                              if( provided_cdd >= request_cdd )
                              {
                                 return inputs;
                              }
                           }
                       }
                       else if( itr->second.claim_func == claim_by_pts && itr->second.amount.unit == 0 )
                       {
                           inputs.push_back( trx_input( _output_index_to_ref[itr->first] ) );
                           total_in += itr->second.amount;
                           auto cdd = itr->second.amount.get_rounded_amount() * (_data.last_scanned_block_num - itr->first.block_idx);
                           if( cdd > 0 ) 
                           {
                              provided_cdd += cdd;
                              req_sigs.insert( _data.recv_pts_addresses[itr->second.as<claim_by_pts_output>().owner] );
                              if( provided_cdd >= request_cdd )
                              {
                                 return inputs;
                              }
                           }
                       }
                   }
                   return inputs;
              }

              /**
               *  Collect inputs that total to at least min_amnt.
               */
              std::vector<trx_input> collect_inputs( const asset& min_amnt, asset& total_in, std::unordered_set<address>& req_sigs )
              {
                   std::vector<trx_input> inputs;
                   for( auto itr = _unspent_outputs.begin(); itr != _unspent_outputs.end(); ++itr )
                   {
                      ilog( "unspent outputs ${o}", ("o",*itr) );
                       if( itr->second.claim_func == claim_by_signature && itr->second.amount.unit == min_amnt.unit )
                       {
                           inputs.push_back( trx_input( _output_index_to_ref[itr->first] ) );
                           total_in += itr->second.amount;
                           req_sigs.insert( itr->second.as<claim_by_signature_output>().owner );
                           ilog( "total in ${in}  min ${min}", ( "in",total_in)("min",min_amnt) );
                           if( total_in.get_rounded_amount() >= min_amnt.get_rounded_amount() )
                           {
                              return inputs;
                           }
                       }
                       else if( itr->second.claim_func == claim_by_pts && itr->second.amount.unit == min_amnt.unit )
                       {
                           inputs.push_back( trx_input( _output_index_to_ref[itr->first] ) );
                           total_in += itr->second.amount;
                           req_sigs.insert( _data.recv_pts_addresses[itr->second.as<claim_by_pts_output>().owner] );
                           ilog( "total in ${in}  min ${min}", ( "in",total_in)("min",min_amnt) );
                           if( total_in.get_rounded_amount() >= min_amnt.get_rounded_amount() )
                           {
                              return inputs;
                           }
                       }
                   }
                   FC_ASSERT( !"Unable to collect sufficient unspent inputs", "", ("min_amnt",min_amnt)("total_collected",total_in) );
              }


              /** completes a transaction signing it and logging it, this is different than wallet::sign_transaction which
               *  merely signs the transaction without checking anything else or storing the transaction.
               **/
              void sign_transaction( signed_transaction& trx, const std::unordered_set<address>& addresses, bool mark_output_as_used = true)
              {
                   trx.stake = _stake;
                   for( auto itr = addresses.begin(); itr != addresses.end(); ++itr )
                   {
                      self->sign_transaction( trx, *itr );
                   }
                   if( mark_output_as_used )
                   {
                      for( auto itr = trx.inputs.begin(); itr != trx.inputs.end(); ++itr )
                      {
                          elog( "MARK AS SPENT ${B}", ("B",itr->output_ref) );
                          self->mark_as_spent( itr->output_ref );
                      }
                      _data.transactions[trx.id()].trx = trx;
                   }
              }
              wallet* self;
      };
   } // namespace detail

   asset wallet::get_fee_rate()
   {
      return my->get_fee_rate();
   }

   wallet::wallet()
   :my( new detail::wallet_impl() )
   {
      my->self = this;
   }

   wallet::~wallet()
   {
      try {
        save();
      } catch ( const fc::exception& e )
      {
         wlog( "unhandled exception while saving wallet ${e}", ("e",e.to_detail_string()) );
      }
   }

   void wallet::open( const fc::path& wallet_dat, const fc::string& password )
   {
       try {
           my->_wallet_dat           = wallet_dat;
           my->_wallet_base_password = password;
           my->_exception_on_open = false;

           FC_ASSERT( fc::exists( wallet_dat ), "", ("wallet_dat",wallet_dat) )

           if( password == std::string() )
           {
               my->_data = fc::json::from_file<bts::wallet::wallet_data>( wallet_dat );
           }
           else
           {
               std::vector<char> plain_txt = aes_load( wallet_dat, fc::sha512::hash( password.c_str(), password.size() ) );
               FC_ASSERT( plain_txt.size() > 0 );
               std::string str( plain_txt.begin(), plain_txt.end() );
               my->_data = fc::json::from_string(str).as<wallet_data>();
           }
       }catch( fc::exception& er ) {
           my->_exception_on_open = true;
           FC_RETHROW_EXCEPTION( er, warn, "unable to load ${wal}", ("wal",wallet_dat) );
       } catch( const std::exception& e ) {
           my->_exception_on_open = true;
           throw  fc::std_exception(
               FC_LOG_MESSAGE( warn, "unable to load ${wal}", ("wal",wallet_dat) ), 
               std::current_exception(), 
               e.what() ) ; 
       } catch( ... ) {  
           my->_exception_on_open = true;
           throw fc::unhandled_exception( 
               FC_LOG_MESSAGE( warn, "unable to load ${wal}", ("wal",wallet_dat)), 
               std::current_exception() ); 
       }
   }

   void wallet::create( const fc::path& wallet_dat, const fc::string& base_password, const fc::string& key_password, bool is_brain )
   { try {
      FC_ASSERT( !fc::exists( wallet_dat ), "", ("wallet_dat",wallet_dat) );
      FC_ASSERT( key_password.size() >= 8 );

      my->_data = wallet_data();

      my->_wallet_dat = wallet_dat;
      my->_wallet_base_password = base_password;
      my->_wallet_key_password  = key_password;
      my->_exception_on_open = false;
      
      if( is_brain )
      {
         FC_ASSERT( base_password.size() >= 8 );
         my->_data.set_base_key( extended_private_key( fc::sha256::hash( key_password.c_str(), key_password.size() ),
                                                             fc::sha256::hash( base_password.c_str(), base_password.size() ) ), key_password );
      }
      else
      {
         my->_data.set_base_key( extended_private_key( fc::ecc::private_key::generate().get_secret(),
                                                       fc::ecc::private_key::generate().get_secret() ), key_password );
      }
      my->_data.set_keys( std::unordered_map<address,fc::ecc::private_key>(), key_password );
      save();
   } FC_RETHROW_EXCEPTIONS( warn, "unable to create wallet ${wal}", ("wal",wallet_dat) ) }

   void wallet::backup_wallet( const fc::path& backup_path )
   { try {
      FC_ASSERT( !fc::exists( backup_path ) );
      auto tmp = my->_wallet_dat;
      my->_wallet_dat = backup_path;
      try {
        save();
        my->_wallet_dat = tmp;
      } 
      catch ( ... )
      {
        my->_wallet_dat = tmp;
        throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "unable to backup to ${path}", ("path",backup_path) ) }

   /**
    *  @note no balances will show up unless you scan the chain after import... perhaps just scan the
    *  genesis block which is the only place where PTS and BTC addresses should be found.
    */
   void wallet::import_bitcoin_wallet( const fc::path& wallet_dat, const std::string& passphrase )
   { try {
    /*
      auto priv_keys = bts::import_bitcoin_wallet(  wallet_dat, passphrase );
   //   ilog( "keys: ${keys}", ("keys",priv_keys) );
      for( auto key : priv_keys )
      {
         auto pts_key = pts_address( key.get_public_key(), false, 0 );
         import_key( key, std::string( pts_key ) );
         my->_data.recv_pts_addresses[ pts_address( key.get_public_key() ) ]           = address( key.get_public_key() );
         my->_data.recv_pts_addresses[ pts_address( key.get_public_key(), false, 0 ) ] = address( key.get_public_key() );
         my->_data.recv_pts_addresses[ pts_address( key.get_public_key(), true, 0 ) ]  = address( key.get_public_key() );
      }
      */
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to import bitcoin wallet ${wallet_dat}", ("wallet_dat",wallet_dat) ) }


   void wallet::save()
   { try {
      ilog( "saving wallet\n" );
      if(my->_exception_on_open)
          return;

      auto wallet_json = fc::json::to_pretty_string( my->_data );
      std::vector<char> data( wallet_json.begin(), wallet_json.end() );

      if( fc::exists( my->_wallet_dat ) )
      {
        auto new_tmp = fc::unique_path();
        auto old_tmp = fc::unique_path();
        if( my->_wallet_base_password.size() )
        {
          fc::aes_save( new_tmp, fc::sha512::hash( my->_wallet_base_password.c_str(), my->_wallet_base_password.size() ), data );
        }
        else
        {
           fc::json::save_to_file( my->_data, new_tmp, true );
        }
        fc::rename( my->_wallet_dat, old_tmp );
        fc::rename( new_tmp, my->_wallet_dat );
        fc::remove( old_tmp );
      }
      else
      {
         if( my->_wallet_base_password.size() != 0 )
         {
            fc::aes_save( my->_wallet_dat, fc::sha512::hash( my->_wallet_base_password.c_str(), my->_wallet_base_password.size() ), data );
         }
         else
         {
            fc::json::save_to_file( my->_data, my->_wallet_dat, true );
         }
      }
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to save wallet ${wallet}", ("wallet",my->_wallet_dat) ) }

   asset wallet::get_balance( asset::type t )
   {
      return my->get_balance(t);
   }

   address   wallet::import_key( const fc::ecc::private_key& key, const std::string& label )
   { try {
      FC_ASSERT( !is_locked() );
      auto keys = my->_data.get_keys( my->_wallet_key_password );
      auto addr = address(key.get_public_key());
      keys[addr] = key;
      my->_data.set_keys( keys, my->_wallet_key_password );
      my->_data.recv_addresses[addr] = label;
      save();
      return addr;
   } FC_RETHROW_EXCEPTIONS( warn, "unable to import private key" ) }

   address   wallet::new_recv_address( const std::string& label )
   { try {
      FC_ASSERT( !is_locked() );
      my->_data.last_used_key++;
      auto base_key = my->_data.get_base_key( my->_wallet_key_password );
      auto new_key = base_key.child( my->_data.last_used_key );
      return import_key(new_key, label);
   } FC_RETHROW_EXCEPTIONS( warn, "unable to create new address with label '${label}'", ("label",label) ) } 

   void wallet::add_send_address( const address& addr, const std::string& label )
   { try {
      my->_data.send_addresses[addr] = label;
      save();
   } FC_RETHROW_EXCEPTIONS( warn, "unable to add send address ${addr} with label ${label}", ("addr",addr)("label",label) ) }

   std::unordered_map<address,std::string> wallet::get_recv_addresses()const
   {
      return my->_data.recv_addresses;
   }
   std::unordered_map<address,std::string> wallet::get_send_addresses()const
   {
      return my->_data.send_addresses;
   }

   void                  wallet::set_fee_rate( const asset& pts_per_byte )
   {
      my->_current_fee_rate = pts_per_byte;
   }

   void                  wallet::unlock_wallet( const std::string& key_password )
   { try {
      my->_data.get_base_key( key_password );
      my->_wallet_key_password = key_password;
   } FC_RETHROW_EXCEPTIONS( warn, "unable to unlock wallet" ) }
   void                  wallet::lock_wallet()
   {
      my->_wallet_base_password = std::string();
   }
   bool   wallet::is_locked()const { return my->_wallet_key_password.size() == 0; }

   signed_transaction    wallet::collect_coindays( uint64_t cdd, uint64_t& cdd_collected, const std::string& label )
   { try {
       auto   change_address = new_recv_address( label );
       std::unordered_set<address> req_sigs; 
       asset  total_in(static_cast<uint64_t>(0ull));
       signed_transaction trx; 
       trx.inputs    = my->collect_coindays( cdd, total_in, req_sigs, cdd_collected );
       asset change = total_in;
       trx.outputs.push_back( trx_output( claim_by_signature_output( change_address ), change) );

       trx.sigs.clear();
       my->sign_transaction( trx, req_sigs, false );

       uint64_t trx_bytes = fc::raw::pack( trx ).size();
       asset    fee( my->_current_fee_rate * trx_bytes );
       FC_ASSERT( total_in > fee );
       trx.outputs.back() = trx_output( claim_by_signature_output( change_address ), change - fee );

       trx.sigs.clear();
       my->sign_transaction(trx, req_sigs, false);
       return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("cdd",cdd)("collected",cdd_collected) ) }


   signed_transaction    wallet::transfer( const asset& amnt, const address& to, const std::string& memo )
   { try {
       auto   change_address = new_recv_address( "change: " + memo );

       std::unordered_set<address> req_sigs; 
       asset  total_in(static_cast<uint64_t>(0ull),amnt.unit);

       signed_transaction trx; 
       trx.inputs    = my->collect_inputs( amnt, total_in, req_sigs );

       asset change = total_in - amnt;

       trx.outputs.push_back( trx_output( claim_by_signature_output( to ), amnt) );
       trx.outputs.push_back( trx_output( claim_by_signature_output( change_address ), change) );

       trx.sigs.clear();
       my->sign_transaction( trx, req_sigs, false );

       uint64_t trx_bytes = fc::raw::pack( trx ).size();
       asset    fee( my->_current_fee_rate * trx_bytes );
       ilog( "required fee ${f}", ( "f",fee ) );

       if( amnt.unit == 0 )
       {
          if( total_in >= amnt + fee )
          {
              change = change - fee;
              trx.outputs.back() = trx_output( claim_by_signature_output( change_address ), change );
              if( change == asset() ) trx.outputs.pop_back(); // no change required
          }
          else
          {
              elog( "NOT ENOUGH TO COVER AMOUNT + FEE... GRAB MORE.." );
              // TODO: this function should be recursive here, but having 2x the fee should be good enough
              fee = fee + fee; // double the fee in this case to cover the growth
              req_sigs.clear();
              total_in = asset();
              trx.inputs = my->collect_inputs( amnt+fee, total_in, req_sigs );
              change =  total_in - amnt - fee;
              trx.outputs.back() = trx_output( claim_by_signature_output( change_address ), change );
              if( change == asset() ) trx.outputs.pop_back(); // no change required
          }
       }
       else /// fee is in bts, but we are transferring something else
       {
           if( change.amount == fc::uint128_t(0) ) trx.outputs.pop_back(); // no change required

           // TODO: this function should be recursive here, but having 2x the fee should be good enough, some
           // transactions may overpay in this case, but this can be optimized later to reduce fees.. for now
           fee = fee + fee; // double the fee in this case to cover the growth
           asset total_fee_in;
           auto extra_in = my->collect_inputs( fee, total_fee_in, req_sigs );
           trx.inputs.insert( trx.inputs.end(), extra_in.begin(), extra_in.end() );
           trx.outputs.push_back( trx_output( claim_by_signature_output( change_address ), total_fee_in - fee ) );
       }

       trx.sigs.clear();
       my->sign_transaction(trx, req_sigs);
       
       return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "${amnt} to ${to}", ("amnt",amnt)("to",to) ) }

   void wallet::mark_as_spent( const output_reference& r )
   {
     // wlog( "MARK SPENT ${s}", ("s",r) );
      auto ref_itr = my->_output_ref_to_index.find(r);
      if( ref_itr == my->_output_ref_to_index.end() ) 
      {
         return;
      }

      auto itr = my->_unspent_outputs.find(ref_itr->second);
      if( itr == my->_unspent_outputs.end() )
      {
          return;
      }
      my->_spent_outputs[ref_itr->second] = itr->second;
      my->_unspent_outputs.erase(ref_itr->second);      
   }

   void wallet::sign_transaction( signed_transaction& trx, const address& addr )
   { try {
      ilog( "Sign ${trx}  ${addr}", ("trx",trx.id())("addr",addr));
      FC_ASSERT( my->_wallet_key_password.size() );
      auto keys = my->_data.get_keys( my->_wallet_key_password );
      auto priv_key_itr = keys.find(addr);
      FC_ASSERT( priv_key_itr != keys.end() );
      trx.sign( priv_key_itr->second );
   } FC_RETHROW_EXCEPTIONS( warn, "unable to sign transaction ${trx} for ${addr}", ("trx",trx)("addr",addr) ) }

   void wallet::sign_transaction( signed_transaction& trx, const std::unordered_set<address>& addresses, bool mark_output_as_used )
   {
      return my->sign_transaction( trx, addresses, mark_output_as_used );
   }

   /** returns all transactions issued */
   std::unordered_map<transaction_id_type, transaction_state> wallet::get_transaction_history()const
   {
      return my->_data.transactions;
   }

   bool wallet::scan_transaction( const signed_transaction& trx, uint32_t block_idx, uint32_t trx_idx )
   {
       bool found = false;
       for( uint32_t in_idx = 0; in_idx < trx.inputs.size(); ++in_idx )
       {
           mark_as_spent( trx.inputs[in_idx].output_ref );
       }

       // for each output
       for( uint32_t out_idx = 0; out_idx < trx.outputs.size(); ++out_idx )
       {
           const trx_output& out   = trx.outputs[out_idx];
           const output_reference  out_ref( trx.id(),out_idx );
           const output_index      oidx( block_idx, trx_idx, out_idx );
           found |= scan_output( out, out_ref, oidx );
       }
       return found;
   }

   /**
    *  Scan the blockchain starting from_block_num until the head block, check every
    *  transaction for inputs or outputs accessable by this wallet.
    *
    *  @return true if a new input was found or output spent
    */
   bool wallet::scan_chain( chain_database& chain, uint32_t from_block_num, scan_progress_callback cb )
   { try {
       bool found = false;
       auto head_block_num = chain.head_block_num();
       // for each block
       for( uint32_t i = from_block_num; i <= head_block_num; ++i )
       {
          auto blk = chain.fetch_digest_block( i );
          // for each transaction
          for( uint32_t trx_idx = 0; trx_idx < blk.trx_ids.size(); ++trx_idx )
          {
              if( cb ) cb( i, head_block_num, trx_idx, blk.trx_ids.size() ); 

              auto trx = chain.fetch_trx( trx_num( i, trx_idx ) ); 
              found |= scan_transaction( trx, i, trx_idx );
          }
          for( uint32_t trx_idx = 0; trx_idx < blk.determinsitic_ids.size(); ++trx_idx )
          {
              auto trx = chain.fetch_trx( trx_num( i, blk.trx_ids.size() + trx_idx ) ); 
              found |= scan_transaction( trx, i, trx_idx );
          }
       }
       set_fee_rate( chain.get_fee_rate() );
       my->_stake                       = chain.get_stake();
       my->_data.last_scanned_block_num = head_block_num;
       return found;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::dump()
   {
       std::cerr<<"===========================================================\n";
       std::cerr<<"Unspent Outputs: \n";
       for( auto itr = my->_unspent_outputs.begin(); itr != my->_unspent_outputs.end(); ++itr )
       {
          std::cerr<<std::setw(13)<<std::string(itr->first)<<"]  ";
          dump_output( itr->second );
          std::cerr<<"\n";
       }
       std::cerr<<"===========================================================\n";
   }
   void wallet::dump_output( const trx_output& out )
   {
       switch( out.claim_func )
       {
          case claim_by_signature:
             std::cerr<<std::string(out.amount)<<" ";
             std::cerr<<"claim_by_signature ";
             std::cerr<< std::string(out.as<claim_by_signature_output>().owner);
             break;
          case claim_by_pts:
             std::cerr<<std::string(out.amount)<<" ";
             std::cerr<<"claim_by_pts ";
             std::cerr<< std::string(out.as<claim_by_pts_output>().owner);
             break;
       }
   }

   bool wallet::is_my_address( const address& a )const
   {
      return my->_data.recv_addresses.find(a)  != my->_data.recv_addresses.end();
   }
   bool wallet::is_my_address( const pts_address& a )const
   {
      return my->_data.recv_pts_addresses.find(a)  != my->_data.recv_pts_addresses.end();
   }

   bool wallet::scan_output( const trx_output& out, const output_reference& out_ref, const bts::wallet::output_index& oidx )
   { try {
      switch( out.claim_func )
      {
         case claim_by_pts:
         {
            if( is_my_address( out.as<claim_by_pts_output>().owner ) )
            {
                cache_output( out, out_ref, oidx );
                return true;
            }
            return false;
         }
         case claim_by_signature:
         {
            if( is_my_address( out.as<claim_by_signature_output>().owner ) )
            {
               cache_output( out, out_ref, oidx );
               return true;
            }
            return false;
         }
         default:
            FC_ASSERT( !"Invalid Claim Type" );
      }
      return false;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }


   output_reference wallet::get_ref_from_output_idx( output_index idx) 
   {
       return my->_output_index_to_ref[idx];
   }

   void wallet::cache_output( const trx_output& out, const output_reference& out_ref, const output_index& oidx )
   {
       my->_output_index_to_ref[oidx]    = out_ref;
       my->_output_ref_to_index[out_ref] = oidx;
       my->_unspent_outputs[oidx]        = out; 
   }
   const std::map<output_index,trx_output>&  wallet::get_unspent_outputs()const
   {
       return my->_unspent_outputs;
   }
   std::vector<trx_input> wallet::collect_inputs( const asset& min_amnt, asset& total_in, std::unordered_set<address>& req_sigs )
   {
      return my->collect_inputs( min_amnt, total_in, req_sigs );
   }

    
} } // namespace bts::wallet
