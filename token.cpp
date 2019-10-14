/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <eosiolib/asset.hpp>
#include <eosiolib/eosio.hpp>
#include <eosiolib/print.hpp>
#include <eosiolib/transaction.hpp>

#include <string>

using namespace eosio;
using namespace std;

CONTRACT token : public contract
{
public:
   using contract::contract;

   token(name receiver, name code, datastream<const char *> ds)
       : contract(receiver, code, ds),
         configtable(_self, _self.value)
   {
   }

#pragma region token

   ACTION init()
   {
      require_auth(_self);

      auto itr = configtable.find(CONFIG_INIT.value);
      eosio_assert(itr == configtable.end() || itr->value == "0", "not allow init.");

      set_config(CONFIG_STAKE_STATUS, "1");
      set_config(CONFIG_ISSUE_STATUS, "1");
      set_config(CONFIG_TRANSFER_STATUS, "1");
      set_config(CONFIG_UNSTAKE_TIME, "86400");

      set_config(CONFIG_INIT, "1");
   }

   ACTION create(name issuer, asset maximum_supply)
   {
      require_auth(_self);

      auto sym = maximum_supply.symbol;
      eosio_assert(sym.is_valid(), "invalid symbol name");
      eosio_assert(maximum_supply.is_valid(), "invalid supply");
      eosio_assert(maximum_supply.amount > 0, "max-supply must be positive");

      stats statstable(_self, sym.code().raw());
      auto existing = statstable.find(sym.code().raw());
      eosio_assert(existing == statstable.end(), "token with symbol already exists");

      statstable.emplace(_self, [&](auto &s) {
         s.supply.symbol = maximum_supply.symbol;
         s.max_supply = maximum_supply;
         s.issuer = issuer;
      });
   }

   ACTION issue(name to, asset quantity, string memo)
   {
      assert_status(CONFIG_ISSUE_STATUS);

      auto sym = quantity.symbol;
      eosio_assert(sym.is_valid(), "invalid symbol name");
      eosio_assert(memo.size() <= 256, "memo has more than 256 bytes");

      stats statstable(_self, sym.code().raw());
      auto existing = statstable.find(sym.code().raw());
      eosio_assert(existing != statstable.end(), "token with symbol does not exist, create token before issue");
      const auto &st = *existing;

      require_auth(st.issuer);
      eosio_assert(quantity.is_valid(), "invalid quantity");
      eosio_assert(quantity.amount > 0, "must issue positive quantity");

      eosio_assert(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
      eosio_assert(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

      statstable.modify(st, same_payer, [&](auto &s) {
         s.supply += quantity;
      });

      add_balance(st.issuer, quantity, st.issuer);

      if (to != st.issuer)
      {
         SEND_INLINE_ACTION(*this, transfer, {{st.issuer, "active"_n}},
                            {st.issuer, to, quantity, memo});
      }
   }

   ACTION transfer(name from,
                   name to,
                   asset quantity,
                   string memo)
   {
      assert_status(CONFIG_TRANSFER_STATUS);
      eosio_assert(from != to, "cannot transfer to self");
      require_auth(from);
      eosio_assert(is_account(to), "to account does not exist");
      auto sym = quantity.symbol.code();
      stats statstable(_self, sym.raw());
      const auto &st = statstable.get(sym.raw());

      require_recipient(from);
      require_recipient(to);

      eosio_assert(quantity.is_valid(), "invalid quantity");
      eosio_assert(quantity.amount > 0, "must transfer positive quantity");
      eosio_assert(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
      eosio_assert(memo.size() <= 256, "memo has more than 256 bytes");

      auto payer = has_auth(to) ? to : from;

      sub_balance(from, quantity);
      add_balance(to, quantity, payer);
   }

   ACTION reduceto(name issuer, asset maximum_supply)
   {
      auto sym = maximum_supply.symbol;
      check(sym.is_valid(), "invalid symbol name");

      stats statstable(get_self(), sym.code().raw());
      auto existing = statstable.find(sym.code().raw());
      check(existing != statstable.end(), "token with symbol does not exist, create token before reduce");
      const auto &st = *existing;
      //check(to == st.issuer, "tokens can only be issued to issuer account");

      require_auth(st.issuer);
      check(maximum_supply.amount >= st.supply.amount, "maximum_supply must greater than current supply.");

      statstable.modify(st, same_payer, [&](auto &s) {
         s.max_supply = maximum_supply;
      });
   }


#pragma endregion

#pragma region TABLE

   TABLE account
   {
      asset balance;
      asset lock_balance;
      asset stake_balance;

      uint64_t primary_key() const { return balance.symbol.code().raw(); }
   };

   TABLE currency_stats
   {
      asset supply;
      asset max_supply;
      name issuer;

      uint64_t primary_key() const { return supply.symbol.code().raw(); }
   };

   TABLE config_table
   {
      name key;
      string value;

      uint64_t primary_key() const { return key.value; }
   };

   TABLE stake_stats
   {
      asset staking;
      asset unstaking;

      uint64_t primary_key() const { return staking.symbol.code().raw(); }
   };

   TABLE staking_log
   {
      name user;
      asset asset;

      uint64_t primary_key() const { return user.value; }
   };

   TABLE unstaking_log
   {
      name user;
      asset asset;
      uint64_t request_time;

      uint64_t primary_key() const { return user.value; }
   };

   typedef multi_index<"config"_n, config_table> configs;

   typedef multi_index<"stakestats"_n, stake_stats> stakestats;
   typedef multi_index<"stakinglog"_n, staking_log> stakinglog;
   typedef multi_index<"unstakinglog"_n, unstaking_log> unstakinglog;

   typedef multi_index<"accounts"_n, account> accounts;
   typedef multi_index<"stat"_n, currency_stats> stats;

#pragma endregion

private:
   configs configtable;

   const name CONFIG_INIT = "init"_n;
   const name CONFIG_STAKE_STATUS = "sstatus"_n;
   const name CONFIG_ISSUE_STATUS = "istatus"_n;
   const name CONFIG_TRANSFER_STATUS = "tstatus"_n;
   const name CONFIG_UNSTAKE_TIME = "unstaketime"_n;

   void sub_balance(name owner, asset value)
   {
      accounts from_acnts(_self, owner.value);

      const auto &from = from_acnts.get(value.symbol.code().raw(), "no balance object found");
      eosio_assert(from.balance.amount - from.lock_balance.amount - from.stake_balance.amount >= value.amount, "overdrawn balance");

      auto payer = has_auth(owner) ? owner : same_payer;

      from_acnts.modify(from, payer, [&](auto &a) {
         a.balance -= value;
      });
   }

   void add_balance(name owner, asset value, name ram_payer)
   {
      accounts to_acnts(_self, owner.value);
      auto to = to_acnts.find(value.symbol.code().raw());
      if (to == to_acnts.end())
      {
         to_acnts.emplace(ram_payer, [&](auto &a) {
            a.balance = value;
            a.lock_balance = asset(0, value.symbol);
            a.stake_balance = asset(0, value.symbol);
         });
      }
      else
      {
         to_acnts.modify(to, same_payer, [&](auto &a) {
            a.balance += value;
         });
      }
   }

   void assert_status(name key)
   {
      auto itr = configtable.find(key.value);
      eosio_assert(itr != configtable.end() && stoul(itr->value) > 0, "current status do not allow doing this action.");
   }

   uint64_t get_unstake_time()
   {
      auto unstake_time = configtable.find(CONFIG_UNSTAKE_TIME.value);
      eosio_assert(unstake_time != configtable.end(), "unstake time not set.");

      return stoull(unstake_time->value);
   }

   void set_config(name key, string value)
   {
      require_auth(_self);

      auto itr = configtable.find(key.value);
      if (itr == configtable.end())
      {
         configtable.emplace(_self, [&](auto &config) {
            config.key = key;
            config.value = value;
         });
      }
      else
      {
         configtable.modify(itr, _self, [&](auto &config) {
            config.value = value;
         });
      }
   }
};

EOSIO_DISPATCH(token, (init)(create)(issue)(transfer)(reduceto))