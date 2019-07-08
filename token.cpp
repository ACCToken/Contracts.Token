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
      eosio_assert(has_auth(from), "has no permission");
      // require_auth(from);
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

      auto payer = has_auth(to) ? to : has_auth(from) ? from : _self;

      sub_balance(from, quantity);
      add_balance(to, quantity, payer);
   }

#pragma endregion

#pragma region lock

   ACTION lock(name account, asset quantity)
   {
      require_auth(_self);

      accounts lock_acnts(_self, account.value);
      const auto &lock = lock_acnts.get(quantity.symbol.code().raw(), "no balance object found");
      eosio_assert(lock.balance.amount - lock.lock_balance.amount - lock.stake_balance.amount >= quantity.amount, "overdrawn balance");

      lock_acnts.modify(lock, same_payer, [&](auto &a) {
         a.lock_balance += quantity;
      });
   }

   ACTION unlock(name account, asset quantity)
   {
      require_auth(_self);

      accounts unlock_acnts(_self, account.value);
      const auto &unlock = unlock_acnts.get(quantity.symbol.code().raw(), "no balance object found");
      eosio_assert(unlock.lock_balance.amount >= quantity.amount, "overdrawn balance");

      unlock_acnts.modify(unlock, same_payer, [&](auto &a) {
         a.lock_balance -= quantity;
      });
   }

#pragma endregion

#pragma region stake

   ACTION stake(name from, asset quantity)
   {
      require_auth(from);
      assert_status(CONFIG_STAKE_STATUS);
      eosio_assert(quantity.is_valid(), "Invalid token transfer");
      eosio_assert(quantity.amount > 0, "Quantity must be positive");

      //query from account balance
      accounts from_acnts(_self, from.value);
      const auto &acc = from_acnts.get(quantity.symbol.code().raw(), "no balance object found");
      eosio_assert(acc.balance.amount - acc.lock_balance.amount - acc.stake_balance.amount >= quantity.amount, "overdrawn balance");

      //save staking log
      stakinglog stakinglogtables(_self, quantity.symbol.code().raw());
      auto staking = stakinglogtables.find(from.value);
      if (staking == stakinglogtables.end())
      {
         stakinglogtables.emplace(_self, [&](auto &stake) {
            stake.user = from;
            stake.asset = quantity;
         });
      }
      else
      {
         stakinglogtables.modify(staking, _self, [&](auto &row) {
            row.asset.amount += quantity.amount;
         });
      }

      //update stats
      stakestats statstable(_self, quantity.symbol.code().raw());
      auto existing = statstable.find(quantity.symbol.code().raw());
      if (existing == statstable.end())
      {
         statstable.emplace(_self, [&](auto &stats) {
            stats.staking = quantity;
            stats.unstaking = asset(0, quantity.symbol);
         });
      }
      else
      {
         statstable.modify(existing, _self, [&](auto &row) {
            row.staking.amount += quantity.amount;
         });
      }

      from_acnts.modify(acc, same_payer, [&](auto &a) {
         a.stake_balance += quantity;
      });
   }

   ACTION unstake(name from, asset quantity, int8_t isinstance)
   {
      eosio_assert(has_auth(from) || has_auth(_self), "has no permission");
      assert_status(CONFIG_STAKE_STATUS);
      eosio_assert(quantity.is_valid(), "Invalid token transfer");
      eosio_assert(quantity.amount > 0, "Quantity must be positive");

      //update staking log
      stakinglog stakinglogtables(_self, quantity.symbol.code().raw());
      auto staking = stakinglogtables.find(from.value);
      eosio_assert(staking != stakinglogtables.end(), "user has not stake yets. ");

      uint64_t stake_amount = staking->asset.amount;
      eosio_assert(stake_amount >= quantity.amount, "stack quantity not enough.");

      if (quantity.amount == stake_amount)
      {
         stakinglogtables.erase(staking);
      }
      else
      {
         stakinglogtables.modify(staking, _self, [&](auto &row) {
            row.asset.amount -= quantity.amount;
         });
      }

      if (isinstance)
      {
         require_auth(_self);

         //update stat
         stakestats statstable(_self, quantity.symbol.code().raw());
         auto stats = statstable.find(quantity.symbol.code().raw());
         statstable.modify(stats, _self, [&](auto &row) {
            row.staking.amount -= quantity.amount;
         });

         accounts from_acnts(_self, from.value);
         const auto &acc = from_acnts.get(quantity.symbol.code().raw(), "no balance object found");
         from_acnts.modify(acc, same_payer, [&](auto &row) {
            row.stake_balance.amount -= quantity.amount;
         });
      }
      else
      {
         //save unstaking log

         unstakinglog unstakinglogtables(_self, quantity.symbol.code().raw());
         auto unstaking = unstakinglogtables.find(from.value);
         if (unstaking == unstakinglogtables.end())
         {
            unstakinglogtables.emplace(_self, [&](auto &unstake) {
               unstake.user = from;
               unstake.asset = quantity;
               unstake.request_time = current_time();
            });
         }
         else
         {
            unstakinglogtables.modify(unstaking, _self, [&](auto &unstake) {
               unstake.asset += quantity;
               unstake.request_time = current_time();
            });
         }

         //update stat
         stakestats statstable(_self, quantity.symbol.code().raw());
         auto stats = statstable.find(quantity.symbol.code().raw());
         statstable.modify(stats, _self, [&](auto &row) {
            row.unstaking.amount += quantity.amount;
            row.staking.amount -= quantity.amount;
         });

         //send defer release
         transaction out{};
         out.actions.emplace_back(permission_level{_self, "active"_n}, _self, "deferrelease"_n, make_tuple(from, quantity.symbol));
         out.delay_sec = get_unstake_time();
         cancel_deferred(from.value);
         out.send(from.value, _self, true);
      }
   }

   ACTION deferrelease(name from, symbol sym)
   {
      require_auth(_self);
      print("deferrelease\n", from, "\n");

      //erase unstaking log
      unstakinglog unstakinglogtables(_self, sym.code().raw());
      auto unstaking = unstakinglogtables.find(from.value);
      uint64_t amount = unstaking->asset.amount;
      unstakinglogtables.erase(unstaking);

      //update stats
      stakestats statstable(_self, sym.code().raw());
      auto stats = statstable.find(sym.code().raw());
      statstable.modify(stats, _self, [&](auto &row) {
         row.unstaking.amount -= amount;
      });
      print("accounts\n", from, "\n");

      accounts from_acnts(_self, from.value);
      const auto &acc = from_acnts.get(sym.code().raw(), "no balance object found");
      from_acnts.modify(acc, same_payer, [&](auto &row) {
         row.stake_balance.amount -= amount;
      });
   }

#pragma endregion

#pragma region config

   ACTION setsstatus(uint8_t new_status)
   {
      set_config(CONFIG_STAKE_STATUS, to_string(new_status));
   }

   ACTION setistatus(uint8_t new_status)
   {
      set_config(CONFIG_ISSUE_STATUS, to_string(new_status));
   }

   ACTION settstatus(uint8_t new_status)
   {
      set_config(CONFIG_TRANSFER_STATUS, to_string(new_status));
   }

   ACTION setust(uint8_t new_time_second)
   {
      set_config(CONFIG_UNSTAKE_TIME, to_string(new_time_second));
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

EOSIO_DISPATCH(token, (init)(create)(issue)(transfer)(deferrelease)(stake)(unstake)(lock)(unlock)(setistatus)(setsstatus)(setust)(settstatus))