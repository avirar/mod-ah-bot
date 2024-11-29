/*
 * Copyright (C) 2008-2010 Trinity <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <unordered_map>
#include <algorithm>
#include <random>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include "ObjectMgr.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "Player.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "DatabaseEnv.h"

#include "AuctionHouseBot.h"
#include "AuctionHouseBotCommon.h"

using namespace std;

AuctionHouseBot::AuctionHouseBot(uint32 account, uint32 id)
{
    _account        = account;
    _id             = id;

    _lastrun_a_sec  = time(NULL);
    _lastrun_h_sec  = time(NULL);
    _lastrun_n_sec  = time(NULL);

    _allianceConfig = NULL;
    _hordeConfig    = NULL;
    _neutralConfig  = NULL;
}

AuctionHouseBot::~AuctionHouseBot()
{
    // Nothing
}

uint32 AuctionHouseBot::getElement(const std::vector<uint32>& vec, int index, uint32 botId, uint32 maxDup, AuctionHouseObject* auctionHouse)
{
    // Ensure index is within bounds
    if (index < 0 || index >= static_cast<int>(vec.size()))
    {
        return 0; // Index out of bounds
    }

    uint32 itemId = vec[index];

    if (maxDup > 0)
    {
        uint32 noStacks = 0;

        // Iterate over the auctions in the auction house
        for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry* Aentry = itr->second;

            // Check if the auction belongs to the bot
            if (Aentry->owner.GetCounter() == botId)
            {
                // Check if the item ID matches
                if (itemId == Aentry->item_template)
                {
                    noStacks++;
                }
            }
        }

        // If the number of stacks exceeds or equals the maximum allowed, return 0
        if (noStacks >= maxDup)
        {
            return 0;
        }
    }

    return itemId;
}


uint32 AuctionHouseBot::getStackCount(AHBConfig* config, uint32 max)
{
    if (max == 1)
    {
        return 1;
    }

    // 
    // Organize the stacks in a pseudo random way
    // 

    if (config->DivisibleStacks)
    {
        uint32 ret = 0;

        if (max % 5 == 0) // 5, 10, 15, 20
        {
            ret = urand(1, 4) * 5;
        }

        if (max % 4 == 0) // 4, 8, 12, 16
        {
            ret = urand(1, 4) * 4;
        }

        if (max % 3 == 0) // 3, 6, 9, 18
        {
            ret = urand(1, 3) * 3;
        }

        if (ret > max)
        {
            ret = max;
        }

        return ret;
    }

    // 
    // Totally random
    // 

    return urand(1, max);
}

uint32 AuctionHouseBot::getElapsedTime(uint32 timeClass)
{
    switch (timeClass)
    {
    case 3:
        return urand(600, 259200); // RANDOM = Between 10 minutes and 3 days

    case 2:
        return urand(1, 6) * 600;  // SHORT = In the range of one hour

    case 1:
        return urand(1, 24) * 3600; // MEDIUM = In the range of one day

    default:
        return urand(1, 3) * 86400; // LONG = More than one day but less than three
    }
}

uint32 AuctionHouseBot::getNofAuctions(AHBConfig* config, AuctionHouseObject* auctionHouse, ObjectGuid guid)
{
    //
    // All the auctions
    //

    if (!config->ConsiderOnlyBotAuctions)
    {
        return auctionHouse->Getcount();
    }

    //
    // Just the one handled by the bot
    //

    uint32 count = 0;

    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry* Aentry = itr->second;

        if (guid == Aentry->owner)
        {
            count++;
            // break;
        }
    }

    return count;
}

// =============================================================================
// This routine performs the bidding operations for the bot
// =============================================================================

void AuctionHouseBot::Buy(Player* AHBplayer, AHBConfig* config, WorldSession* session)
{
    //
    // Check if disabled
    //

    if (!config->AHBBuyer)
    {
        return;
    }

    //
    // Retrieve items not owner by the bot and not bought by the bot
    //

    QueryResult result = CharacterDatabase.Query("SELECT id FROM auctionhouse WHERE itemowner<>{} AND buyguid<>{}", _id, _id);

    if (!result)
    {
        return;
    }

    if (result->GetRowCount() == 0)
    {
        return;
    }

    //
    // Fetches content of selected AH to look for possible bids
    //

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    std::set<uint32>    possibleBids;

    do
    {
        uint32 tmpdata = result->Fetch()->Get<uint32>();
        possibleBids.insert(tmpdata);
    } while (result->NextRow());

    //
    // If it's not possible to bid stop here
    //

    if (possibleBids.empty())
    {
        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "AHBot [{}]: no auctions to bid on has been recovered", _id);
        }

        return;
    }

    //
    // Perform the operation for a maximum amount of bids attempts configured
    //

    for (uint32 count = 1; count <= config->GetBidsPerInterval(); ++count)
    {
        //
        // Choose a random auction from possible auctions
        //

        uint32 randBid = urand(0, possibleBids.size() - 1);

        std::set<uint32>::iterator it = possibleBids.begin();
        std::advance(it, randBid);

        AuctionEntry* auction = auctionHouse->GetAuction(*it);

        //
        // Prevent to bid again on the same auction
        //

        possibleBids.erase(randBid);

        if (!auction)
        {
            continue;
        }

        //
        // Prevent from buying items from the other bots
        //

        if (gBotsId.find(auction->owner.GetCounter()) != gBotsId.end())
        {
            continue;
        }

        //
        // Get the item information
        //

        Item* pItem = sAuctionMgr->GetAItem(auction->item_guid);

        if (!pItem)
        {
            if (config->DebugOutBuyer)
            {
                LOG_ERROR("module", "AHBot [{}]: item {} doesn't exist, perhaps bought already?", _id, auction->item_guid.ToString());
            }

            continue;
        }

        //
        // Get the item prototype
        //

        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(auction->item_template);

        //
        // Check which price we have to use, startbid or if it is bidded already
        //

        uint32 currentprice;

        if (auction->bid)
        {
            currentprice = auction->bid;
        }
        else
        {
            currentprice = auction->startbid;
        }

        //
        // Prepare portion from maximum bid
        //

        double      bidrate = static_cast<double>(urand(1, 100)) / 100;
        long double bidMax  = 0;

        //
        // Check that bid has an acceptable value and take bid based on vendorprice, stacksize and quality
        //

        if (config->BuyMethod)
        {
            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                if (currentprice < prototype->SellPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality))
                {
                    bidMax = prototype->SellPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }
        else
        {
            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                if (currentprice < prototype->BuyPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality))
                {
                    bidMax = prototype->BuyPrice * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }

        //
        // Recalculate the bid depending on the type of the item
        //

        switch (prototype->Class)
        {
            // ammo
        case 6:
            bidMax = 0;
            break;
        default:
            break;
        }

        //
        // Test the computed bid
        //

        if (bidMax == 0)
        {
            continue;
        }

        //
        // Calculate our bid
        //

        long double bidvalue = currentprice + ((bidMax - currentprice) * bidrate);
        uint32      bidprice = static_cast<uint32>(bidvalue);

        //
        // Check our bid is high enough to be valid. If not, correct it to minimum.
        //

        if ((currentprice + auction->GetAuctionOutBid()) > bidprice)
        {
            bidprice = currentprice + auction->GetAuctionOutBid();
        }

        //
        // Print out debug info
        //

        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "-------------------------------------------------");
            LOG_INFO("module", "AHBot [{}]: Info for Auction #{}:", _id, auction->Id);
            LOG_INFO("module", "AHBot [{}]: AuctionHouse: {}"     , _id, auction->GetHouseId());
            LOG_INFO("module", "AHBot [{}]: Owner: {}"            , _id, auction->owner.ToString());
            LOG_INFO("module", "AHBot [{}]: Bidder: {}"           , _id, auction->bidder.ToString());
            LOG_INFO("module", "AHBot [{}]: Starting Bid: {}"     , _id, auction->startbid);
            LOG_INFO("module", "AHBot [{}]: Current Bid: {}"      , _id, currentprice);
            LOG_INFO("module", "AHBot [{}]: Buyout: {}"           , _id, auction->buyout);
            LOG_INFO("module", "AHBot [{}]: Deposit: {}"          , _id, auction->deposit);
            LOG_INFO("module", "AHBot [{}]: Expire Time: {}"      , _id, uint32(auction->expire_time));
            LOG_INFO("module", "AHBot [{}]: Bid Rate: {}"         , _id, bidrate);
            LOG_INFO("module", "AHBot [{}]: Bid Max: {}"          , _id, bidMax);
            LOG_INFO("module", "AHBot [{}]: Bid Value: {}"        , _id, bidvalue);
            LOG_INFO("module", "AHBot [{}]: Bid Price: {}"        , _id, bidprice);
            LOG_INFO("module", "AHBot [{}]: Item GUID: {}"        , _id, auction->item_guid.ToString());
            LOG_INFO("module", "AHBot [{}]: Item Template: {}"    , _id, auction->item_template);
            LOG_INFO("module", "AHBot [{}]: Item Info:");
            LOG_INFO("module", "AHBot [{}]: Item ID: {}"          , _id, prototype->ItemId);
            LOG_INFO("module", "AHBot [{}]: Buy Price: {}"        , _id, prototype->BuyPrice);
            LOG_INFO("module", "AHBot [{}]: Sell Price: {}"       , _id, prototype->SellPrice);
            LOG_INFO("module", "AHBot [{}]: Bonding: {}"          , _id, prototype->Bonding);
            LOG_INFO("module", "AHBot [{}]: Quality: {}"          , _id, prototype->Quality);
            LOG_INFO("module", "AHBot [{}]: Item Level: {}"       , _id, prototype->ItemLevel);
            LOG_INFO("module", "AHBot [{}]: Ammo Type: {}"        , _id, prototype->AmmoType);
            LOG_INFO("module", "-------------------------------------------------");
        }

        //
        // Check whether we do normal bid, or buyout
        //

        bool bought = false;

        if ((bidprice < auction->buyout) || (auction->buyout == 0))
        {
            //
            // Perform a new bid on the auction
            //
        
            if (auction->bidder)
            {
                if (auction->bidder != AHBplayer->GetGUID())
                {
                    //
                    // Mail to last bidder and return their money
                    //
        
                    auto trans = CharacterDatabase.BeginTransaction();
        
                    sAuctionMgr->SendAuctionOutbiddedMail(auction, bidprice, session->GetPlayer(), trans);
                    CharacterDatabase.CommitTransaction  (trans);
                }
            }
        
            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = bidprice;
        
            //
            // Save the auction into database
            //
        
            CharacterDatabase.Execute("UPDATE auctionhouse SET buyguid = '{}', lastbid = '{}' WHERE id = '{}'", auction->bidder.GetCounter(), auction->bid, auction->Id);
        }
        else
        {
            bought = true;

            //
            // Perform the buyout
            //

            auto trans = CharacterDatabase.BeginTransaction();

            if ((auction->bidder) && (AHBplayer->GetGUID() != auction->bidder))
            {
                //
                // Send the mail to the last bidder
                //

                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, session->GetPlayer(), trans);
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = auction->buyout;

            // 
            // Send mails to buyer & seller
            // 

            sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
            sAuctionMgr->SendAuctionWonMail       (auction, trans);

            // 
            // Removes any trace of the item
            // 

            auction->DeleteFromDB(trans);

            sAuctionMgr->RemoveAItem   (auction->item_guid);
            auctionHouse->RemoveAuction(auction);

            CharacterDatabase.CommitTransaction(trans);
        }

        //
        // Tracing
        //

        if (config->TraceBuyer)
        {
            if (bought)
            {
                LOG_INFO("module", "AHBot [{}]: Bought , id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
            else
            {
                LOG_INFO("module", "AHBot [{}]: New bid, id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
        }
    }
}

// =============================================================================
// This routine performs the selling operations for the bot
// =============================================================================

void AuctionHouseBot::Sell(Player* AHBplayer, AHBConfig* config)
{
    // Check if disabled
    if (!config->AHBSeller)
        return;

    // Check the given limits
    uint32 minItems = config->GetMinItems();
    uint32 maxItems = config->GetMaxItems();

    if (maxItems == 0)
        return;

    // Retrieve the auction house situation
    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntry(config->GetAHFID());
    if (!ahEntry)
        return;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    if (!auctionHouse)
        return;

    auctionHouse->Update();

    // Check if we are clear to proceed
    uint32 auctions = getNofAuctions(config, auctionHouse, AHBplayer->GetGUID());
    uint32 items = 0;

    if (auctions >= minItems)
    {
        if (config->DebugOutSeller)
            LOG_ERROR("module", "AHBot [{}]: Auctions above minimum", _id);
        return;
    }

    if (auctions >= maxItems)
    {
        if (config->DebugOutSeller)
            LOG_ERROR("module", "AHBot [{}]: Auctions at or above maximum", _id);
        return;
    }

    if (auctions == 0)
    {
        // If no bot auctions exist, populate the auction house up to minItems / 10
        items = minItems / 10;
    }
    else if ((maxItems - auctions) >= config->ItemsPerCycle)
    {
        items = config->ItemsPerCycle;
    }
    else
    {
        items = (maxItems - auctions);
    }

    // Retrieve the configuration for this run
    std::unordered_map<uint32, uint32> maxCounts = {
        {AHB_GREY_TG,   config->GetMaximum(AHB_GREY_TG)},
        {AHB_WHITE_TG,  config->GetMaximum(AHB_WHITE_TG)},
        {AHB_GREEN_TG,  config->GetMaximum(AHB_GREEN_TG)},
        {AHB_BLUE_TG,   config->GetMaximum(AHB_BLUE_TG)},
        {AHB_PURPLE_TG, config->GetMaximum(AHB_PURPLE_TG)},
        {AHB_ORANGE_TG, config->GetMaximum(AHB_ORANGE_TG)},
        {AHB_YELLOW_TG, config->GetMaximum(AHB_YELLOW_TG)},
        {AHB_GREY_I,    config->GetMaximum(AHB_GREY_I)},
        {AHB_WHITE_I,   config->GetMaximum(AHB_WHITE_I)},
        {AHB_GREEN_I,   config->GetMaximum(AHB_GREEN_I)},
        {AHB_BLUE_I,    config->GetMaximum(AHB_BLUE_I)},
        {AHB_PURPLE_I,  config->GetMaximum(AHB_PURPLE_I)},
        {AHB_ORANGE_I,  config->GetMaximum(AHB_ORANGE_I)},
        {AHB_YELLOW_I,  config->GetMaximum(AHB_YELLOW_I)}
    };

    std::unordered_map<uint32, uint32> currentCounts = {
        {AHB_GREY_TG,   config->GetItemCounts(AHB_GREY_TG)},
        {AHB_WHITE_TG,  config->GetItemCounts(AHB_WHITE_TG)},
        {AHB_GREEN_TG,  config->GetItemCounts(AHB_GREEN_TG)},
        {AHB_BLUE_TG,   config->GetItemCounts(AHB_BLUE_TG)},
        {AHB_PURPLE_TG, config->GetItemCounts(AHB_PURPLE_TG)},
        {AHB_ORANGE_TG, config->GetItemCounts(AHB_ORANGE_TG)},
        {AHB_YELLOW_TG, config->GetItemCounts(AHB_YELLOW_TG)},
        {AHB_GREY_I,    config->GetItemCounts(AHB_GREY_I)},
        {AHB_WHITE_I,   config->GetItemCounts(AHB_WHITE_I)},
        {AHB_GREEN_I,   config->GetItemCounts(AHB_GREEN_I)},
        {AHB_BLUE_I,    config->GetItemCounts(AHB_BLUE_I)},
        {AHB_PURPLE_I,  config->GetItemCounts(AHB_PURPLE_I)},
        {AHB_ORANGE_I,  config->GetItemCounts(AHB_ORANGE_I)},
        {AHB_YELLOW_I,  config->GetItemCounts(AHB_YELLOW_I)}
    };

    // Define item bins
    struct ItemBin
    {
        std::vector<uint32>& bin;
        uint32& currentCount;
        uint32 maxCount;
        uint32 choice;
        uint32 adjustedDuplicates;
    };

    // Initialize bins
    std::vector<ItemBin> itemBins = {
        // Poor Quality Items
        {config->GreyItemsBin, currentCounts[AHB_GREY_I], maxCounts[AHB_GREY_I], 0, config->DuplicatesCount},
        {config->GreyTradeGoodsBin, currentCounts[AHB_GREY_TG], maxCounts[AHB_GREY_TG], 7, config->DuplicatesCount},
        // Normal Quality Items
        {config->WhiteItemsBin, currentCounts[AHB_WHITE_I], maxCounts[AHB_WHITE_I], 1, config->DuplicatesCount},
        {config->WhiteTradeGoodsBin, currentCounts[AHB_WHITE_TG], maxCounts[AHB_WHITE_TG], 8, config->DuplicatesCount},
        // Uncommon Quality Items
        {config->GreenItemsBin, currentCounts[AHB_GREEN_I], maxCounts[AHB_GREEN_I], 2, config->DuplicatesCount},
        {config->GreenTradeGoodsBin, currentCounts[AHB_GREEN_TG], maxCounts[AHB_GREEN_TG], 9, config->DuplicatesCount},
        // Rare Quality Items (adjusted duplicates)
        {config->BlueItemsBin, currentCounts[AHB_BLUE_I], maxCounts[AHB_BLUE_I], 3, config->DuplicatesCount / 2},
        {config->BlueTradeGoodsBin, currentCounts[AHB_BLUE_TG], maxCounts[AHB_BLUE_TG], 10, config->DuplicatesCount / 2},
        // Epic Quality Items (adjusted duplicates)
        {config->PurpleItemsBin, currentCounts[AHB_PURPLE_I], maxCounts[AHB_PURPLE_I], 4, config->DuplicatesCount / 4},
        {config->PurpleTradeGoodsBin, currentCounts[AHB_PURPLE_TG], maxCounts[AHB_PURPLE_TG], 11, config->DuplicatesCount / 4},
        // Legendary Quality Items
        {config->OrangeItemsBin, currentCounts[AHB_ORANGE_I], maxCounts[AHB_ORANGE_I], 5, config->DuplicatesCount},
        {config->OrangeTradeGoodsBin, currentCounts[AHB_ORANGE_TG], maxCounts[AHB_ORANGE_TG], 12, config->DuplicatesCount},
        // Artifact Quality Items
        {config->YellowItemsBin, currentCounts[AHB_YELLOW_I], maxCounts[AHB_YELLOW_I], 6, config->DuplicatesCount},
        {config->YellowTradeGoodsBin, currentCounts[AHB_YELLOW_TG], maxCounts[AHB_YELLOW_TG], 13, config->DuplicatesCount}
    };

    // Loop variables
    uint32 noSold   = 0; // Tracing counter
    uint32 binEmpty = 0; // Tracing counter
    uint32 loopBrk  = 0; // Tracing counter
    uint32 err      = 0; // Tracing counter

    // Start a transaction outside the loop
    auto trans = CharacterDatabase.BeginTransaction();

    for (uint32 cnt = 1; cnt <= items; cnt++)
    {
        uint32 itemID = 0;
        uint32 choice = 0;
        uint32 loopbreaker = 0;

        while (itemID == 0 && loopbreaker <= AUCTION_HOUSE_BOT_LOOP_BREAKER)
        {
            loopbreaker++;

            // Shuffle itemBins to add randomness
            std::vector<size_t> indices(itemBins.size());
            std::iota(indices.begin(), indices.end(), 0); // Fill with 0, 1, ..., itemBins.size() - 1
            
            std::shuffle(indices.begin(), indices.end(), std::mt19937{std::random_device{}()});
            
            // Then, iterate over indices instead of itemBins
            for (size_t idx : indices)
            {
                auto& bin = itemBins[idx];
                {
                    if (bin.bin.empty() || bin.currentCount >= bin.maxCount)
                        continue;
    
                    uint32 randomIndex = urand(0, bin.bin.size() - 1);
                    itemID = getElement(bin.bin, randomIndex, _id, bin.adjustedDuplicates, auctionHouse);
    
                    if (itemID != 0)
                    {
                        choice = bin.choice;
                        break;
                    }
                }
            }

            if (itemID == 0)
            {
                binEmpty++;
                if (config->DebugOutSeller)
                    LOG_ERROR("module", "AHBot [{}]: No item could be selected from the bins", _id);
                break;
            }
        }

        if (itemID == 0 || loopbreaker > AUCTION_HOUSE_BOT_LOOP_BREAKER)
        {
            loopBrk++;
            continue;
        }

        // Retrieve information about the selected item
        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemID);

        if (!prototype)
        {
            err++;
            if (config->DebugOutSeller)
                LOG_ERROR("module", "AHBot [{}]: Could not get prototype of item {}", _id, itemID);
            continue;
        }

        Item* item = Item::CreateItem(itemID, 1, AHBplayer);

        if (!item)
        {
            err++;
            if (config->DebugOutSeller)
                LOG_ERROR("module", "AHBot [{}]: Could not create item from prototype {}", _id, itemID);
            continue;
        }

        // Start interacting with the item by adding a random property
        item->AddToUpdateQueueOf(AHBplayer);

        uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(itemID);

        if (randomPropertyId != 0)
            item->SetItemRandomProperties(randomPropertyId);

        if (prototype->Quality > AHB_MAX_QUALITY)
        {
            err++;
            if (config->DebugOutSeller)
                LOG_ERROR("module", "AHBot [{}]: Quality {} TOO HIGH for item {}", _id, prototype->Quality, itemID);

            item->RemoveFromUpdateQueueOf(AHBplayer);
            continue;
        }

        // Determine the price
        uint64 buyoutPrice = CalculateItemPrice(prototype, config);
        uint64 bidPrice = buyoutPrice * urand(config->GetMinBidPrice(prototype->Quality), config->GetMaxBidPrice(prototype->Quality)) / 100;

        // Determine the stack size
        uint32 stackCount = DetermineStackSize(prototype, config);

        item->SetCount(stackCount);

        // Determine the auction time
        uint32 etime = getElapsedTime(config->ElapsingTimeClass);

        // Determine the deposit
        uint32 dep = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, item, stackCount);

        // Perform the auction
        AuctionEntry* auctionEntry = new AuctionEntry();
        auctionEntry->Id                = sObjectMgr->GenerateAuctionID();
        auctionEntry->houseId           = config->GetAHID();
        auctionEntry->item_guid         = item->GetGUID();
        auctionEntry->item_template     = item->GetEntry();
        auctionEntry->itemCount         = item->GetCount();
        auctionEntry->owner             = AHBplayer->GetGUID();
        auctionEntry->startbid          = bidPrice * stackCount;
        auctionEntry->buyout            = buyoutPrice * stackCount;
        auctionEntry->bid               = 0;
        auctionEntry->deposit           = dep;
        auctionEntry->expire_time       = (time_t)etime + time(NULL);
        auctionEntry->auctionHouseEntry = ahEntry;

        item->SaveToDB(trans);
        item->RemoveFromUpdateQueueOf(AHBplayer);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(auctionEntry);
        auctionEntry->SaveToDB(trans);

        // Increment the number of items present in the auction
        auto it = currentCounts.find(choice);
        if (it != currentCounts.end())
            ++(it->second);

        noSold++;

        if (config->TraceSeller)
        {
            std::string formattedExpireTime;
            if (auctionEntry->expire_time == 0)
            {
                formattedExpireTime = "Never";
            }
            else
            {
                std::tm* timeInfo = std::localtime(&auctionEntry->expire_time);
                if (timeInfo)
                {
                    std::ostringstream oss;
                    oss << std::put_time(timeInfo, "%Y-%m-%d %H:%M:%S");
                    formattedExpireTime = oss.str();
                }
                else
                {
                    formattedExpireTime = "Invalid Time";
                }
            }

            // Log the new auction entry with expire_time
            LOG_INFO("module", "AHBot [{}]: New stack ah={}, id={}, stack={}, bid={}, buyout={}, expire_time={}",
                _id,
                config->GetAHID(),
                itemID,
                stackCount,
                auctionEntry->startbid,
                auctionEntry->buyout,
                formattedExpireTime);
        }
    }

    // Commit the transaction after processing all items
    CharacterDatabase.CommitTransaction(trans);

    if (config->TraceSeller)
    {
        LOG_INFO("module", "AHBot [{}]: auctionhouse {}, req={}, sold={}, loopBrk={}, binEmpty={}, err={}",
            _id, config->GetAHID(), items, noSold, loopBrk, binEmpty, err);
    }
}

uint64 AuctionHouseBot::CalculateItemPrice(const ItemTemplate* prototype, AHBConfig* config)
{
    uint64 price = 0;

    if (config->SellAtMarketPrice)
    {
        price = config->GetItemPrice(prototype->ItemId);
    }

    if (price == 0)
    {
        price = config->SellMethod ? prototype->BuyPrice : prototype->SellPrice;
    }

    if (config->SellZeroPriceItems && price == 0)
    {
        // Determine base price based on item level
        int basePrice = urand(prototype->ItemLevel, prototype->ItemLevel + 10) * 10;

        // Determine the quality multiplier
        int qualityMultiplier = GetQualityMultiplier(prototype->Quality);

        // Apply the quality multiplier to the base price
        price = basePrice * qualityMultiplier;
    }

    if (price == 0)
    {
        // Handle error or assign a default price
        if (config->DebugOutSeller)
            LOG_ERROR("module", "AHBot [{}]: Could not determine a price for item {} of quality {}", _id, prototype->ItemId, prototype->Quality);
        return 0;
    }

    // Apply random modifiers based on quality
    price *= urand(config->GetMinPrice(prototype->Quality), config->GetMaxPrice(prototype->Quality));
    price /= 25;

    return price;
}

int AuctionHouseBot::GetQualityMultiplier(uint32 quality)
{
    switch (quality)
    {
        case ITEM_QUALITY_POOR:      return 1;
        case ITEM_QUALITY_NORMAL:    return 2;
        case ITEM_QUALITY_UNCOMMON:  return 5;
        case ITEM_QUALITY_RARE:      return 10;
        case ITEM_QUALITY_EPIC:      return 20;
        case ITEM_QUALITY_LEGENDARY:
        case ITEM_QUALITY_ARTIFACT:  return 50;
        default:                     return 1;
    }
}

uint32 AuctionHouseBot::DetermineStackSize(ItemTemplate const* prototype, AHBConfig* config)
{
    uint32 itemMaxStack = prototype->GetMaxStackSize();
    uint32 configMaxStack = config->GetMaxStack(prototype->Quality);
    
    // If configMaxStack is 0, default to item's inherent max stack size
    if (configMaxStack == 0)
    {
        configMaxStack = itemMaxStack;
    }
    
    // Calculate maxStack as the minimum of itemMaxStack and configMaxStack
    uint32 maxStack = std::min(itemMaxStack, configMaxStack);
    
    // Retrieve the stack divisor based on item quality
    uint32 stackDivisor = GetStackDivisor(prototype->Quality);
    
    // Adjust maxStack using the divisor only if the result is an integer
    if (stackDivisor > 1 && maxStack % stackDivisor == 0)
    {
        maxStack = maxStack / stackDivisor;
    }
    
    // Ensure maxStack is at least 1
    if (maxStack < 1)
    {
        maxStack = 1;
    }
    
    // Debug output
    if (config->DebugOutSeller)
    {
        LOG_DEBUG("module", "AHBot [{}]: Item {}: itemMaxStack={}, configMaxStack={}, stackDivisor={}, adjusted maxStack={}",
            _id, prototype->ItemId, itemMaxStack, configMaxStack, stackDivisor, maxStack);
    }
    
    // Proceed with determining the stack count
    if (maxStack > 1)
    {
        uint32 stackCount = urand(1, maxStack);
        
        // Optional: Favor full stacks
        if (urand(1, 100) <= 30) // 30% chance for full stack
            stackCount = maxStack;
        
        if (config->DebugOutSeller)
        {
            LOG_DEBUG("module", "AHBot [{}]: Determined stack size for item {}: {}", _id, prototype->ItemId, stackCount);
        }
        
        return stackCount;
    }
    else
    {
        return 1;
    }
}

uint32 AuctionHouseBot::GetStackDivisor(uint32 quality)
{
    switch (quality)
    {
        case ITEM_QUALITY_RARE:     return 2;
        case ITEM_QUALITY_EPIC:     return 4;
        default:                    return 1;
    }
}


// =============================================================================
// Perform an update cycle
// =============================================================================

void AuctionHouseBot::Update()
{
    time_t _newrun = time(NULL);

    if (!_allianceConfig && !_hordeConfig && !_neutralConfig)
    {
        return;
    }

    // Prepare for operation
    std::string accountName = "AuctionHouseBot" + std::to_string(_account);

    WorldSession _session(_account, std::move(accountName), nullptr, SEC_PLAYER, sWorld->getIntConfig(CONFIG_EXPANSION), 0, LOCALE_enUS, 0, false, false, 0);

    Player _AHBplayer(&_session);
    _AHBplayer.Initialize(_id);

    ObjectAccessor::AddObject(&_AHBplayer);

    switch (_nextFactionToProcess)
    {
        case 0:
            // Alliance
            if (_allianceConfig)
            {
                Sell(&_AHBplayer, _allianceConfig);

                if (((_newrun - _lastrun_a_sec) >= (_allianceConfig->GetBiddingInterval() * MINUTE)) && (_allianceConfig->GetBidsPerInterval() > 0))
                {
                    Buy(&_AHBplayer, _allianceConfig, &_session);
                    _lastrun_a_sec = _newrun;
                }
            }
            break;
        case 1:
            // Horde
            if (_hordeConfig)
            {
                Sell(&_AHBplayer, _hordeConfig);

                if (((_newrun - _lastrun_h_sec) >= (_hordeConfig->GetBiddingInterval() * MINUTE)) && (_hordeConfig->GetBidsPerInterval() > 0))
                {
                    Buy(&_AHBplayer, _hordeConfig, &_session);
                    _lastrun_h_sec = _newrun;
                }
            }
            break;
        case 2:
            // Neutral
            if (_neutralConfig)
            {
                Sell(&_AHBplayer, _neutralConfig);

                if (((_newrun - _lastrun_n_sec) >= (_neutralConfig->GetBiddingInterval() * MINUTE)) && (_neutralConfig->GetBidsPerInterval() > 0))
                {
                    Buy(&_AHBplayer, _neutralConfig, &_session);
                    _lastrun_n_sec = _newrun;
                }
            }
            break;
    }

    // Update the faction to process next time
    _nextFactionToProcess = (_nextFactionToProcess + 1) % 3;

    ObjectAccessor::RemoveObject(&_AHBplayer);
}


// =============================================================================
// Execute commands coming from the console
// =============================================================================

void AuctionHouseBot::Commands(AHBotCommand command, uint32 ahMapID, uint32 col, char* args)
{
    //
    // Retrieve the auction house configuration
    //

    AHBConfig *config = NULL;

    switch (ahMapID)
    {
    case 2:
        config = _allianceConfig;
        break;
    case 6:
        config = _hordeConfig;
        break;
    default:
        config = _neutralConfig;
        break;
    }

    //
    // Retrive the item quality
    //

    std::string color;

    switch (col)
    {
    case AHB_GREY:
        color = "grey";
        break;
    case AHB_WHITE:
        color = "white";
        break;
    case AHB_GREEN:
        color = "green";
        break;
    case AHB_BLUE:
        color = "blue";
        break;
    case AHB_PURPLE:
        color = "purple";
        break;
    case AHB_ORANGE:
        color = "orange";
        break;
    case AHB_YELLOW:
        color = "yellow";
        break;
    default:
        break;
    }

    //
    // Perform the command
    //

    switch (command)
    {
    case AHBotCommand::buyer:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBBuyer = false;
            _hordeConfig->AHBBuyer    = false;
            _neutralConfig->AHBBuyer  = false;
        }
        else
        {
            _allianceConfig->AHBBuyer = true;
            _hordeConfig->AHBBuyer    = true;
            _neutralConfig->AHBBuyer  = true;
        }

        break;
    }
    case AHBotCommand::seller:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBSeller = false;
            _hordeConfig->AHBSeller    = false;
            _neutralConfig->AHBSeller  = false;
        }
        else
        {
            _allianceConfig->AHBSeller = true;
            _hordeConfig->AHBSeller    = true;
            _neutralConfig->AHBSeller  = true;
        }

        break;
    }
    case AHBotCommand::useMarketPrice:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->SellAtMarketPrice = false;
            _hordeConfig->SellAtMarketPrice    = false;
            _neutralConfig->SellAtMarketPrice  = false;
        }
        else
        {
            _allianceConfig->SellAtMarketPrice = true;
            _hordeConfig->SellAtMarketPrice    = true;
            _neutralConfig->SellAtMarketPrice  = true;
        }

        break;
    }
    case AHBotCommand::ahexpire:
    {
        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

        AuctionHouseObject::AuctionEntryMap::iterator itr;
        itr = auctionHouse->GetAuctionsBegin();

        //
        // Iterate through all the autions and if they belong to the bot, make them expired
        //

        while (itr != auctionHouse->GetAuctionsEnd())
        {
            if (itr->second->owner.GetCounter() == _id)
            {
                // Expired NOW.
                itr->second->expire_time = GameTime::GetGameTime().count();

                uint32 id                = itr->second->Id;
                uint32 expire_time       = itr->second->expire_time;

                CharacterDatabase.Execute("UPDATE auctionhouse SET time = '{}' WHERE id = '{}'", expire_time, id);
            }

            ++itr;
        }

        break;
    }
    case AHBotCommand::minitems:
    {
        char * param1   = strtok(args, " ");
        uint32 minItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minitems = '{}' WHERE auctionhouse = '{}'", minItems, ahMapID);

        config->SetMinItems(minItems);

        break;
    }
    case AHBotCommand::maxitems:
    {
        char * param1   = strtok(args, " ");
        uint32 maxItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxitems = '{}' WHERE auctionhouse = '{}'", maxItems, ahMapID);

        config->SetMaxItems(maxItems);
        config->CalculatePercents();
        break;
    }
    case AHBotCommand::percentages:
    {
        char * param1   = strtok(args, " ");
        char * param2   = strtok(NULL, " ");
        char * param3   = strtok(NULL, " ");
        char * param4   = strtok(NULL, " ");
        char * param5   = strtok(NULL, " ");
        char * param6   = strtok(NULL, " ");
        char * param7   = strtok(NULL, " ");
        char * param8   = strtok(NULL, " ");
        char * param9   = strtok(NULL, " ");
        char * param10  = strtok(NULL, " ");
        char * param11  = strtok(NULL, " ");
        char * param12  = strtok(NULL, " ");
        char * param13  = strtok(NULL, " ");
        char * param14  = strtok(NULL, " ");

        uint32 greytg   = (uint32) strtoul(param1, NULL, 0);
        uint32 whitetg  = (uint32) strtoul(param2, NULL, 0);
        uint32 greentg  = (uint32) strtoul(param3, NULL, 0);
        uint32 bluetg   = (uint32) strtoul(param4, NULL, 0);
        uint32 purpletg = (uint32) strtoul(param5, NULL, 0);
        uint32 orangetg = (uint32) strtoul(param6, NULL, 0);
        uint32 yellowtg = (uint32) strtoul(param7, NULL, 0);
        uint32 greyi    = (uint32) strtoul(param8, NULL, 0);
        uint32 whitei   = (uint32) strtoul(param9, NULL, 0);
        uint32 greeni   = (uint32) strtoul(param10, NULL, 0);
        uint32 bluei    = (uint32) strtoul(param11, NULL, 0);
        uint32 purplei  = (uint32) strtoul(param12, NULL, 0);
        uint32 orangei  = (uint32) strtoul(param13, NULL, 0);
        uint32 yellowi  = (uint32) strtoul(param14, NULL, 0);

        //
        // Setup the percentage in the configuration first, so validity test can be performed
        //

        config->SetPercentages(greytg, whitetg, greentg, bluetg, purpletg, orangetg, yellowtg, greyi, whitei, greeni, bluei, purplei, orangei, yellowi);

        //
        // Save the results into the database (after the tests)
        //

        auto trans = WorldDatabase.BeginTransaction();

        trans->Append("UPDATE mod_auctionhousebot SET percentgreytradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhitetradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreentradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentbluetradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpletradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangetradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowtradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreyitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhiteitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreenitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentblueitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpleitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangeitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_I) , ahMapID);

        WorldDatabase.CommitTransaction(trans);

        break;
    }
    case AHBotCommand::minprice:
    {
        char * param1   = strtok(args, " ");
        uint32 minPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minprice{} = '{}' WHERE auctionhouse = '{}'", color, minPrice, ahMapID);

        config->SetMinPrice(col, minPrice);

        break;
    }
    case AHBotCommand::maxprice:
    {
        char * param1   = strtok(args, " ");
        uint32 maxPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxprice{} = '{}' WHERE auctionhouse = '{}'", color, maxPrice, ahMapID);

        config->SetMaxPrice(col, maxPrice);

        break;
    }
    case AHBotCommand::minbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 minBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minbidprice{} = '{}' WHERE auctionhouse = '{}'", color, minBidPrice, ahMapID);

        config->SetMinBidPrice(col, minBidPrice);

        break;
    }
    case AHBotCommand::maxbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 maxBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxbidprice{} = '{}' WHERE auctionhouse = '{}'", color, maxBidPrice, ahMapID);

        config->SetMaxBidPrice(col, maxBidPrice);

        break;
    }
    case AHBotCommand::maxstack:
    {
        char * param1   = strtok(args, " ");
        uint32 maxStack = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxstack{} = '{}' WHERE auctionhouse = '{}'", color, maxStack, ahMapID);

        config->SetMaxStack(col, maxStack);

        break;
    }
    case AHBotCommand::buyerprice:
    {
        char * param1     = strtok(args, " ");
        uint32 buyerPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerprice{} = '{}' WHERE auctionhouse = '{}'", color, buyerPrice, ahMapID);

        config->SetBuyerPrice(col, buyerPrice);

        break;
    }
    case AHBotCommand::bidinterval:
    {
        char * param1      = strtok(args, " ");
        uint32 bidInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbiddinginterval = '{}' WHERE auctionhouse = '{}'", bidInterval, ahMapID);

        config->SetBiddingInterval(bidInterval);

        break;
    }
    case AHBotCommand::bidsperinterval:
    {
        char * param1          = strtok(args, " ");
        uint32 bidsPerInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbidsperinterval = '{}' WHERE auctionhouse = '{}'", bidsPerInterval, ahMapID);

        config->SetBidsPerInterval(bidsPerInterval);

        break;
    }
    default:
        break;
    }
}

// =============================================================================
// Initialization of the bot
// =============================================================================

void AuctionHouseBot::Initialize(AHBConfig* allianceConfig, AHBConfig* hordeConfig, AHBConfig* neutralConfig)
{
    // 
    // Save the pointer for the configurations
    // 

    _allianceConfig = allianceConfig;
    _hordeConfig    = hordeConfig;
    _neutralConfig  = neutralConfig;

    //
    // Done
    //

    LOG_INFO("module", "AHBot [{}]: initialization complete\n", uint32(_id));
}
