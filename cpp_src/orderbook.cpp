#include <iostream>
#include <map>
#include <unordered_map>
#include <deque>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace std;

enum OrderType { BUY, SELL };
enum OrderStatus { ACTIVE, FILLED, PARTIALLY_FILLED, CANCELLED };
enum OrderVariant { LIMIT, MARKET, IOC, FOK }; // Added order variants
enum MarketStatus { NORMAL_TRADING, CIRCUIT_HALT, PRE_OPEN_AUCTION, CLOSED };
enum CircuitLevel { NONE, LEVEL_1, LEVEL_2, LEVEL_3 };

// Forward declaration
class OrderBook;

class Order {
public:
    int id;
    OrderType type;
    OrderVariant variant;
    double price;
    int quantity;
    int filled_quantity;
    OrderStatus status;
    time_t timestamp;
    string symbol;
    time_t expiry;  // For GTD orders

    Order() : id(0), type(BUY), variant(LIMIT), price(0), quantity(0), filled_quantity(0),
             status(ACTIVE), timestamp(time(0)), expiry(0) {}

    Order(int id, OrderType type, OrderVariant variant, double price, int quantity, string sym, time_t exp = 0)
        : id(id),
          type(type),
          variant(variant),
          price(price),
          quantity(quantity),
          filled_quantity(0),
          status(ACTIVE),
          timestamp(time(0)),
          symbol(sym),
          expiry(exp) {}

    int getRemainingQuantity() const {
        return quantity - filled_quantity;
    }

    string getTimestamp() const {
        char buffer[26];
        strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", localtime(&timestamp));
        return string(buffer);
    }

    string getStatusString() const {
        switch(status) {
            case ACTIVE: return "ACTIVE";
            case FILLED: return "FILLED";
            case PARTIALLY_FILLED: return "PARTIALLY_FILLED";
            case CANCELLED: return "CANCELLED";
            default: return "UNKNOWN";
        }
    }

    string getVariantString() const {
        switch(variant) {
            case LIMIT: return "LIMIT";
            case MARKET: return "MARKET";
            case IOC: return "IOC";
            case FOK: return "FOK";
            default: return "UNKNOWN";
        }
    }
};

// Trade class to represent executed trades
class Trade {
public:
    int buyOrderId;
    int sellOrderId;
    string symbol;
    double price;
    int quantity;
    time_t timestamp;

    Trade(int buyId, int sellId, const string& sym, double p, int qty)
        : buyOrderId(buyId), sellOrderId(sellId), symbol(sym),
          price(p), quantity(qty), timestamp(time(0)) {}

    string getTimestamp() const {
        char buffer[26];
        strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", localtime(&timestamp));
        return string(buffer);
    }
};

class MarketCircuitBreaker {
private:
    double referenceValue;
    double currentValue;
    CircuitLevel currentLevel;
    MarketStatus status;
    time_t haltStartTime;
    time_t haltEndTime;

public:
    MarketCircuitBreaker(double refValue)
        : referenceValue(refValue), currentValue(refValue),
          currentLevel(NONE), status(NORMAL_TRADING),
          haltStartTime(0), haltEndTime(0) {}

    bool updateMarketValue(double newValue, time_t currentTime) {
        currentValue = newValue;
        double percentChange = ((newValue - referenceValue) / referenceValue) * 100.0;

        // Check if we need to trigger circuit breaker
        if (status == NORMAL_TRADING) {
            if (percentChange <= -20.0) {
                triggerCircuitBreaker(LEVEL_3, currentTime);
                return true;
            } else if (percentChange <= -15.0) {
                triggerCircuitBreaker(LEVEL_2, currentTime);
                return true;
            } else if (percentChange <= -10.0) {
                triggerCircuitBreaker(LEVEL_1, currentTime);
                return true;
            }
        } else if (status == CIRCUIT_HALT) {
            // Check if halt period is over
            if (currentTime >= haltEndTime) {
                status = PRE_OPEN_AUCTION;
                // Pre-open auction always lasts 15 minutes
                haltEndTime = currentTime + 15 * 60; // 15 minutes in seconds
            }
        } else if (status == PRE_OPEN_AUCTION) {
            // Check if pre-open auction is over
            if (currentTime >= haltEndTime) {
                status = NORMAL_TRADING;
                currentLevel = NONE;
            }
        }

        return false;
    }

    MarketStatus getStatus() const {
        return status;
    }

    time_t getHaltEndTime() const {
        return haltEndTime;
    }

private:
    void triggerCircuitBreaker(CircuitLevel level, time_t currentTime) {
        currentLevel = level;
        status = CIRCUIT_HALT;
        haltStartTime = currentTime;

        struct tm* timeinfo = localtime(&currentTime);
        int hour = timeinfo->tm_hour;
        int minute = timeinfo->tm_min;

        // Convert to minutes since market open for easier comparison (assuming 9:00 AM open)
        int minutesSinceOpen = (hour - 9) * 60 + minute;

        // Apply halt duration based on the level and time of day
        // Level 1 (10% drop)
        if (level == LEVEL_1) {
            if (minutesSinceOpen < 240) { // Before 1:00 PM (4 hours after open)
                haltEndTime = currentTime + 45 * 60; // 45 minutes
            } else if (minutesSinceOpen < 330) { // Between 1:00 PM and 2:30 PM
                haltEndTime = currentTime + 15 * 60; // 15 minutes
            } else { // After 2:30 PM
                // No halt
                status = NORMAL_TRADING;
                currentLevel = NONE;
            }
        }
        // Level 2 (15% drop)
        else if (level == LEVEL_2) {
            if (minutesSinceOpen < 240) { // Before 1:00 PM
                haltEndTime = currentTime + 105 * 60; // 1 hour 45 minutes
            } else if (minutesSinceOpen < 300) { // Between 1:00 PM and 2:00 PM
                haltEndTime = currentTime + 45 * 60; // 45 minutes
            } else { // After 2:00 PM
                haltEndTime = 0; // Remainder of the day
                status = CLOSED;
            }
        }
        // Level 3 (20% drop)
        else if (level == LEVEL_3) {
            haltEndTime = 0; // Remainder of the day
            status = CLOSED;
        }
    }
};

class OrderBook {
private:
    // Price -> deque of Order pointers (for time priority)
    // Using map for price levels (automatically sorted)
    unordered_map<string, map<double, deque<shared_ptr<Order>>, greater<double>>> buyOrders;  // Descending for buys
    unordered_map<string, map<double, deque<shared_ptr<Order>>, less<double>>> sellOrders;    // Ascending for sells

    // All orders by ID for quick lookup
    unordered_map<int, shared_ptr<Order>> orderMap;

    // Symbol-level locks for better concurrency
    unordered_map<string, shared_mutex> symbolMutexes;
    mutex orderIdMutex;

    // Circuit breaker for market-wide halts
    MarketCircuitBreaker circuitBreaker;

    // Individual stock price bands (dynamic circuit breakers)
    unordered_map<string, double> referencePrices;
    unordered_map<string, double> priceBandPercentages;

    int nextOrderId;
    vector<shared_ptr<Trade>> tradeHistory;

public:
    OrderBook() : nextOrderId(1), circuitBreaker(17500.0) {
        // Initialize with default reference index value (e.g., Nifty50 at 17500)
    }

    void setStockPriceBand(const string& symbol, double referencePrice, double bandPercentage) {
        referencePrices[symbol] = referencePrice;
        priceBandPercentages[symbol] = bandPercentage;
    }

    void updateIndexValue(double newValue, time_t currentTime) {
        bool circuitTriggered = circuitBreaker.updateMarketValue(newValue, currentTime);
        if (circuitTriggered) {
            cout << "MARKET CIRCUIT BREAKER TRIGGERED!" << endl;
            MarketStatus status = circuitBreaker.getStatus();
            if (status == CIRCUIT_HALT) {
                time_t endTime = circuitBreaker.getHaltEndTime();
                char buffer[26];
                strftime(buffer, 26, "%H:%M:%S", localtime(&endTime));
                cout << "Trading halted until: " << buffer << endl;
            } else if (status == CLOSED) {
                cout << "Trading halted for the remainder of the day." << endl;
            }
        }
    }

    // Market Order - executes immediately at best available price
    int placeMarketOrder(OrderType type, int quantity, const string& symbol) {
        // Check market status
        MarketStatus marketStatus = circuitBreaker.getStatus();
        if (marketStatus != NORMAL_TRADING) {
            cout << "Market order rejected: Market is not in normal trading mode." << endl;
            return -1;
        }

        // Generate unique order ID
        lock_guard<mutex> idLock(orderIdMutex);
        int orderId = nextOrderId++;

        // For market orders, price is set to 0 initially (placeholder)
        auto newOrder = make_shared<Order>(orderId, type, MARKET, 0.0, quantity, symbol);

        // Store in ID map
        orderMap[orderId] = newOrder;

        cout << "Market Order Placed: " << (type == BUY ? "BUY" : "SELL")
             << " " << quantity << " " << symbol << " at MARKET"
             << " (ID: " << orderId << ")" << endl;

        // Execute market order immediately
        executeMarketOrder(newOrder);

        return orderId;
    }

    // IOC (Immediate or Cancel) Order
    int placeIOCOrder(OrderType type, double price, int quantity, const string& symbol) {
        // Check market status
        MarketStatus marketStatus = circuitBreaker.getStatus();
        if (marketStatus != NORMAL_TRADING) {
            cout << "IOC order rejected: Market is not in normal trading mode." << endl;
            return -1;
        }

        // Generate unique order ID
        lock_guard<mutex> idLock(orderIdMutex);
        int orderId = nextOrderId++;

        auto newOrder = make_shared<Order>(orderId, type, IOC, price, quantity, symbol);

        // Store in ID map
        orderMap[orderId] = newOrder;

        cout << "IOC Order Placed: " << (type == BUY ? "BUY" : "SELL")
             << " " << quantity << " " << symbol << " at $" << fixed << setprecision(2)
             << price << " (ID: " << orderId << ")" << endl;

        // Execute IOC order immediately
        executeIOCOrder(newOrder);

        return orderId;
    }

    // FOK (Fill or Kill) Order
    int placeFOKOrder(OrderType type, double price, int quantity, const string& symbol) {
        // Check market status
        MarketStatus marketStatus = circuitBreaker.getStatus();
        if (marketStatus != NORMAL_TRADING) {
            cout << "FOK order rejected: Market is not in normal trading mode." << endl;
            return -1;
        }

        // Generate unique order ID
        lock_guard<mutex> idLock(orderIdMutex);
        int orderId = nextOrderId++;

        auto newOrder = make_shared<Order>(orderId, type, FOK, price, quantity, symbol);

        // Store in ID map
        orderMap[orderId] = newOrder;

        cout << "FOK Order Placed: " << (type == BUY ? "BUY" : "SELL")
             << " " << quantity << " " << symbol << " at $" << fixed << setprecision(2)
             << price << " (ID: " << orderId << ")" << endl;

        // Execute FOK order
        if (!executeFOKOrder(newOrder)) {
            // If not fully executed, cancel the order
            newOrder->status = CANCELLED;
            cout << "FOK Order " << orderId << " cancelled: Could not fill completely." << endl;
        }

        return orderId;
    }

    // Regular limit order (enhanced version to include OrderVariant)
    int placeOrder(OrderType type, double price, int quantity, const string& symbol) {
        return placeOrder(type, LIMIT, price, quantity, symbol);
    }

    // General order placement function that handles all order types
    int placeOrder(OrderType type, OrderVariant variant, double price, int quantity, const string& symbol) {
        // For market orders, delegate to dedicated function
        if (variant == MARKET) {
            return placeMarketOrder(type, quantity, symbol);
        }
        // For IOC orders, delegate to dedicated function
        else if (variant == IOC) {
            return placeIOCOrder(type, price, quantity, symbol);
        }
        // For FOK orders, delegate to dedicated function
        else if (variant == FOK) {
            return placeFOKOrder(type, price, quantity, symbol);
        }

        // Regular limit order processing
        // Check if market is halted due to circuit breaker
        MarketStatus marketStatus = circuitBreaker.getStatus();
        if (marketStatus == CIRCUIT_HALT || marketStatus == CLOSED) {
            cout << "Order rejected: Market is currently halted due to circuit breaker." << endl;
            return -1;
        }

        // Check if pre-open auction is in progress (would have different order matching)
        if (marketStatus == PRE_OPEN_AUCTION) {
            cout << "Order queued for pre-open auction session." << endl;
            // In a real system, this would queue the order for the auction matching
            // For simplicity, we'll just reject it
            return -1;
        }

        // Check stock-specific price bands
        if (referencePrices.find(symbol) != referencePrices.end()) {
            double refPrice = referencePrices[symbol];
            double bandPct = priceBandPercentages[symbol];
            double upperLimit = refPrice * (1 + bandPct/100.0);
            double lowerLimit = refPrice * (1 - bandPct/100.0);

            if (price > upperLimit || price < lowerLimit) {
                cout << "Order rejected: Price " << price << " is outside the allowed band of "
                     << lowerLimit << " to " << upperLimit << " for " << symbol << endl;
                return -1;
            }
        }

        // Generate unique order ID
        {
            lock_guard<mutex> idLock(orderIdMutex);
            int orderId = nextOrderId++;

            auto newOrder = make_shared<Order>(orderId, type, variant, price, quantity, symbol);

            // Store in ID map (needs to happen before we release the lock)
            orderMap[orderId] = newOrder;

            // Lock the specific symbol
            {
                unique_lock<shared_mutex> symbolLock(getOrCreateSymbolMutex(symbol));

                // Add to the appropriate price level
                if (type == BUY) {
                    buyOrders[symbol][price].push_back(newOrder);
                } else {
                    sellOrders[symbol][price].push_back(newOrder);
                }
            }

            cout << "Order Placed: " << (type == BUY ? "BUY" : "SELL")
                 << " " << quantity << " " << symbol << " at $" << fixed << setprecision(2)
                 << price << " (" << newOrder->getVariantString() << ", ID: " << orderId << ")" << endl;

            // Match orders after placing a new one - this will acquire its own lock
            matchOrders(symbol);

            return orderId;
        }
    }

    void matchOrders(const string& symbol) {
        // Create a new lock - don't assume the calling function has locked
        unique_lock<shared_mutex> lock(getOrCreateSymbolMutex(symbol));

        auto& buyBook = buyOrders[symbol];
        auto& sellBook = sellOrders[symbol];

        bool matchFound;
        do {
            matchFound = false;

            // Check if we have any buy and sell orders
            if (!buyBook.empty() && !sellBook.empty()) {
                // Get best buy price (highest) and best sell price (lowest)
                auto bestBuyIt = buyBook.begin();
                auto bestSellIt = sellBook.begin();

                // If the best buy price >= best sell price, we have a match
                if (bestBuyIt->first >= bestSellIt->first &&
                    !bestBuyIt->second.empty() && !bestSellIt->second.empty()) {

                    // Get the oldest orders at these price levels
                    auto buyOrder = bestBuyIt->second.front();
                    auto sellOrder = bestSellIt->second.front();

                    // Continue only if both orders are active
                    if (buyOrder->status != CANCELLED && sellOrder->status != CANCELLED) {
                        // Determine match quantity and execute the trade
                        int matchQuantity = min(buyOrder->getRemainingQuantity(), sellOrder->getRemainingQuantity());
                        double tradePrice = sellOrder->price; // Match at sell price (taker pays)

                        // Record the trade
                        auto trade = make_shared<Trade>(buyOrder->id, sellOrder->id, symbol, tradePrice, matchQuantity);
                        tradeHistory.push_back(trade);

                        // Update order quantities
                        buyOrder->filled_quantity += matchQuantity;
                        sellOrder->filled_quantity += matchQuantity;

                        // Update order status
                        updateOrderStatus(buyOrder);
                        updateOrderStatus(sellOrder);

                        cout << "\nTrade Executed: " << matchQuantity << " " << symbol
                             << " at $" << fixed << setprecision(2) << tradePrice
                             << " (Buy: " << buyOrder->id << ", Sell: " << sellOrder->id << ")" << endl;

                        // Clean up fully filled or cancelled orders
                        if (buyOrder->status == FILLED || buyOrder->status == CANCELLED) {
                            bestBuyIt->second.pop_front();
                            if (bestBuyIt->second.empty()) {
                                buyBook.erase(bestBuyIt);
                            }
                        }

                        if (sellOrder->status == FILLED || sellOrder->status == CANCELLED) {
                            bestSellIt->second.pop_front();
                            if (bestSellIt->second.empty()) {
                                sellBook.erase(bestSellIt);
                            }
                        }

                        matchFound = true;
                    } else {
                        // Remove cancelled orders
                        if (buyOrder->status == CANCELLED) {
                            bestBuyIt->second.pop_front();
                            if (bestBuyIt->second.empty()) {
                                buyBook.erase(bestBuyIt);
                            }
                        }

                        if (sellOrder->status == CANCELLED) {
                            bestSellIt->second.pop_front();
                            if (bestSellIt->second.empty()) {
                                sellBook.erase(bestSellIt);
                            }
                        }

                        matchFound = true; // Continue checking after removing cancelled orders
                    }
                }
            }
        } while (matchFound);
    }

    bool cancelOrder(int orderId) {
        // Find the order first
        auto it = orderMap.find(orderId);
        if (it == orderMap.end()) {
            cout << "Order not found: " << orderId << endl;
            return false;
        }

        auto order = it->second;

        // Lock the specific symbol
        unique_lock<shared_mutex> lock(getOrCreateSymbolMutex(order->symbol));

        if (order->status == FILLED) {
            cout << "Cannot cancel filled order: " << orderId << endl;
            return false;
        }

        // Mark as cancelled
        order->status = CANCELLED;

        cout << "Order cancelled: " << orderId << endl;
        return true;
    }

    void printOrderBook(const string& symbol) {
        // Read-only lock for the symbol
        shared_lock<shared_mutex> lock(getOrCreateSymbolMutex(symbol));

        cout << "\nOrder Book for " << symbol << ":" << endl;
        cout << "-------------------" << endl;

        cout << "Buy Orders (highest first):" << endl;
        if (buyOrders.find(symbol) != buyOrders.end()) {
            for (const auto& priceLevelPair : buyOrders[symbol]) {
                double price = priceLevelPair.first;
                const auto& orders = priceLevelPair.second;

                for (const auto& order : orders) {
                    if (order->status == ACTIVE || order->status == PARTIALLY_FILLED) {
                        cout << "Price: $" << fixed << setprecision(2) << price
                             << ", Qty: " << order->getRemainingQuantity()
                             << ", ID: " << order->id
                             << ", Type: " << order->getVariantString()
                             << ", Status: " << order->getStatusString()
                             << ", Time: " << order->getTimestamp() << endl;
                    }
                }
            }
        }

        cout << "\nSell Orders (lowest first):" << endl;
        if (sellOrders.find(symbol) != sellOrders.end()) {
            for (const auto& priceLevelPair : sellOrders[symbol]) {
                double price = priceLevelPair.first;
                const auto& orders = priceLevelPair.second;

                for (const auto& order : orders) {
                    if (order->status == ACTIVE || order->status == PARTIALLY_FILLED) {
                        cout << "Price: $" << fixed << setprecision(2) << price
                             << ", Qty: " << order->getRemainingQuantity()
                             << ", ID: " << order->id
                             << ", Type: " << order->getVariantString()
                             << ", Status: " << order->getStatusString()
                             << ", Time: " << order->getTimestamp() << endl;
                    }
                }
            }
        }
    }

    void printTradeHistory(const string& symbol) {
        cout << "\nTrade History for " << symbol << ":" << endl;
        cout << "------------------------" << endl;

        for (const auto& trade : tradeHistory) {
            if (trade->symbol == symbol) {
                cout << "Time: " << trade->getTimestamp()
                     << ", Qty: " << trade->quantity
                     << ", Price: $" << fixed << setprecision(2) << trade->price
                     << ", Buy ID: " << trade->buyOrderId
                     << ", Sell ID: " << trade->sellOrderId << endl;
            }
        }
    }

private:
    // Execute market order (immediately match with best available prices)
    void executeMarketOrder(shared_ptr<Order>& order) {
    unique_lock<shared_mutex> lock(getOrCreateSymbolMutex(order->symbol));

    // Determine which side of the book to match against
    if (order->type == BUY) {
        auto& sellBook = sellOrders[order->symbol];
        int remainingQty = order->quantity;

        // Go through sell orders from lowest to highest price
        while (remainingQty > 0 && !sellBook.empty()) {
            // Use a copy of the iterator, not a reference
            auto bestPriceIt = sellBook.begin();
            double matchPrice = bestPriceIt->first;
            auto& ordersAtPrice = bestPriceIt->second;

            while (remainingQty > 0 && !ordersAtPrice.empty()) {
                auto& sellOrder = ordersAtPrice.front();

                // Skip cancelled orders
                if (sellOrder->status == CANCELLED) {
                    ordersAtPrice.pop_front();
                    continue;
                }

                // Determine match quantity
                int matchQty = min(remainingQty, sellOrder->getRemainingQuantity());

                // Execute the trade
                auto trade = make_shared<Trade>(order->id, sellOrder->id, order->symbol, matchPrice, matchQty);
                tradeHistory.push_back(trade);

                // Update quantities
                remainingQty -= matchQty;
                order->filled_quantity += matchQty;
                sellOrder->filled_quantity += matchQty;

                // Update order status
                updateOrderStatus(sellOrder);

                cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                     << " at $" << fixed << setprecision(2) << matchPrice
                     << " (Buy: " << order->id << " [MARKET], Sell: " << sellOrder->id << ")" << endl;

                // Remove filled sell orders
                if (sellOrder->status == FILLED) {
                    ordersAtPrice.pop_front();
                } else {
                    break; // Partially filled but no more quantity available for this market order
                }
            }

            // Clean up empty price levels
            if (ordersAtPrice.empty()) {
                sellBook.erase(bestPriceIt);
            }
        }

        // Update market order status
        updateOrderStatus(order);

        // If market order couldn't be completely filled
        if (order->status != FILLED) {
            cout << "Market Buy Order " << order->id << " partially filled: "
                 << order->filled_quantity << " of " << order->quantity
                 << " shares. Remaining quantity cancelled." << endl;

            // Market orders can't rest in the book
            order->status = PARTIALLY_FILLED;
        }
    } else { // SELL market order
        auto& buyBook = buyOrders[order->symbol];
        int remainingQty = order->quantity;

        // Go through buy orders from highest to lowest price
        while (remainingQty > 0 && !buyBook.empty()) {
            auto bestPriceIt = buyBook.begin();
            double matchPrice = bestPriceIt->first;
            auto& ordersAtPrice = bestPriceIt->second;

            while (remainingQty > 0 && !ordersAtPrice.empty()) {
                auto& buyOrder = ordersAtPrice.front();

                // Skip cancelled orders
                if (buyOrder->status == CANCELLED) {
                    ordersAtPrice.pop_front();
                    continue;
                }

                // Determine match quantity
                int matchQty = min(remainingQty, buyOrder->getRemainingQuantity());

                // Execute the trade
                auto trade = make_shared<Trade>(buyOrder->id, order->id, order->symbol, matchPrice, matchQty);
                tradeHistory.push_back(trade);

                // Update quantities
                remainingQty -= matchQty;
                order->filled_quantity += matchQty;
                buyOrder->filled_quantity += matchQty;

                // Update order status
                updateOrderStatus(buyOrder);

                cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                     << " at $" << fixed << setprecision(2) << matchPrice
                     << " (Buy: " << buyOrder->id << ", Sell: " << order->id << " [MARKET])" << endl;

                // Remove filled buy orders
                if (buyOrder->status == FILLED) {
                    ordersAtPrice.pop_front();
                } else {
                    break; // Partially filled but no more quantity available for this market order
                }
            }

            // Clean up empty price levels
            if (ordersAtPrice.empty()) {
                buyBook.erase(bestPriceIt);
            }
        }

        // Update market order status
        updateOrderStatus(order);

        // If market order couldn't be completely filled
        if (order->status != FILLED) {
            cout << "Market Sell Order " << order->id << " partially filled: "
                 << order->filled_quantity << " of " << order->quantity
                 << " shares. Remaining quantity cancelled." << endl;

            // Market orders can't rest in the book
            order->status = PARTIALLY_FILLED;
        }
    }
}

   // Execute IOC (Immediate or Cancel) order - continuation
    void executeIOCOrder(shared_ptr<Order>& order) {
        unique_lock<shared_mutex> lock(getOrCreateSymbolMutex(order->symbol));

        // Try to match as much as possible immediately
        if (order->type == BUY) {
            auto& sellBook = sellOrders[order->symbol];
            int remainingQty = order->quantity;

            // Go through sell orders with price <= order price
            for (auto sellIt = sellBook.begin();
                 sellIt != sellBook.end() && remainingQty > 0 && sellIt->first <= order->price;
                 /* increment in loop */) {

                double matchPrice = sellIt->first;
                auto& ordersAtPrice = sellIt->second;

                for (auto orderIt = ordersAtPrice.begin();
                     orderIt != ordersAtPrice.end() && remainingQty > 0;
                     /* increment in loop */) {

                    auto& sellOrder = *orderIt;

                    // Skip cancelled orders
                    if (sellOrder->status == CANCELLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                        continue;
                    }

                    // Determine match quantity
                    int matchQty = min(remainingQty, sellOrder->getRemainingQuantity());

                    // Execute the trade
                    auto trade = make_shared<Trade>(order->id, sellOrder->id, order->symbol, matchPrice, matchQty);
                    tradeHistory.push_back(trade);

                    // Update quantities
                    remainingQty -= matchQty;
                    order->filled_quantity += matchQty;
                    sellOrder->filled_quantity += matchQty;

                    // Update order status
                    updateOrderStatus(sellOrder);

                    cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                         << " at $" << fixed << setprecision(2) << matchPrice
                         << " (Buy: " << order->id << " [IOC], Sell: " << sellOrder->id << ")" << endl;

                    // Remove filled sell orders
                    if (sellOrder->status == FILLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                    } else {
                        ++orderIt;
                    }
                }

                // Clean up empty price levels
                if (ordersAtPrice.empty()) {
                    sellIt = sellBook.erase(sellIt);
                } else {
                    ++sellIt;
                }
            }

            // Update IOC order status
            updateOrderStatus(order);

            // If IOC order couldn't be completely filled, cancel the remainder
            if (order->status != FILLED) {
                cout << "IOC Buy Order " << order->id << " partially filled: "
                     << order->filled_quantity << " of " << order->quantity
                     << " shares. Remaining quantity cancelled." << endl;

                // IOC orders that aren't fully filled are cancelled
                if (order->status == PARTIALLY_FILLED) {
                    order->status = CANCELLED;
                }
            }
        } else { // SELL IOC order
            auto& buyBook = buyOrders[order->symbol];
            int remainingQty = order->quantity;

            // Go through buy orders with price >= order price
            for (auto buyIt = buyBook.begin();
                 buyIt != buyBook.end() && remainingQty > 0 && buyIt->first >= order->price;
                 /* increment in loop */) {

                double matchPrice = buyIt->first;
                auto& ordersAtPrice = buyIt->second;

                for (auto orderIt = ordersAtPrice.begin();
                     orderIt != ordersAtPrice.end() && remainingQty > 0;
                     /* increment in loop */) {

                    auto& buyOrder = *orderIt;

                    // Skip cancelled orders
                    if (buyOrder->status == CANCELLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                        continue;
                    }

                    // Determine match quantity
                    int matchQty = min(remainingQty, buyOrder->getRemainingQuantity());

                    // Execute the trade
                    auto trade = make_shared<Trade>(buyOrder->id, order->id, order->symbol, matchPrice, matchQty);
                    tradeHistory.push_back(trade);

                    // Update quantities
                    remainingQty -= matchQty;
                    order->filled_quantity += matchQty;
                    buyOrder->filled_quantity += matchQty;

                    // Update order status
                    updateOrderStatus(buyOrder);

                    cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                         << " at $" << fixed << setprecision(2) << matchPrice
                         << " (Buy: " << buyOrder->id << ", Sell: " << order->id << " [IOC])" << endl;

                    // Remove filled buy orders
                    if (buyOrder->status == FILLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                    } else {
                        ++orderIt;
                    }
                }

                // Clean up empty price levels
                if (ordersAtPrice.empty()) {
                    buyIt = buyBook.erase(buyIt);
                } else {
                    ++buyIt;
                }
            }

            // Update IOC order status
            updateOrderStatus(order);

            // If IOC order couldn't be completely filled, cancel the remainder
            if (order->status != FILLED) {
                cout << "IOC Sell Order " << order->id << " partially filled: "
                     << order->filled_quantity << " of " << order->quantity
                     << " shares. Remaining quantity cancelled." << endl;

                // IOC orders that aren't fully filled are cancelled
                if (order->status == PARTIALLY_FILLED) {
                    order->status = CANCELLED;
                }
            }
        }
    }

    // Execute FOK (Fill or Kill) order - must be filled completely or cancelled
    bool executeFOKOrder(shared_ptr<Order>& order) {
        unique_lock<shared_mutex> lock(getOrCreateSymbolMutex(order->symbol));

        // First check if the order can be filled completely
        bool canFillCompletely = false;
        int availableQty = 0;

        if (order->type == BUY) {
            auto& sellBook = sellOrders[order->symbol];

            // Check available quantity at acceptable prices
            for (auto sellIt = sellBook.begin();
                 sellIt != sellBook.end() && sellIt->first <= order->price;
                 ++sellIt) {

                for (auto& sellOrder : sellIt->second) {
                    if (sellOrder->status != CANCELLED) {
                        availableQty += sellOrder->getRemainingQuantity();
                    }
                }

                if (availableQty >= order->quantity) {
                    canFillCompletely = true;
                    break;
                }
            }

        } else { // SELL FOK order
            auto& buyBook = buyOrders[order->symbol];

            // Check available quantity at acceptable prices
            for (auto buyIt = buyBook.begin();
                 buyIt != buyBook.end() && buyIt->first >= order->price;
                 ++buyIt) {

                for (auto& buyOrder : buyIt->second) {
                    if (buyOrder->status != CANCELLED) {
                        availableQty += buyOrder->getRemainingQuantity();
                    }
                }

                if (availableQty >= order->quantity) {
                    canFillCompletely = true;
                    break;
                }
            }
        }

        // If can't fill completely, return false
        if (!canFillCompletely) {
            return false;
        }

        // If we can fill completely, execute the trades
        if (order->type == BUY) {
            auto& sellBook = sellOrders[order->symbol];
            int remainingQty = order->quantity;

            // Go through sell orders from lowest to highest price
            for (auto sellIt = sellBook.begin();
                 sellIt != sellBook.end() && remainingQty > 0 && sellIt->first <= order->price;
                 /* increment in loop */) {

                double matchPrice = sellIt->first;
                auto& ordersAtPrice = sellIt->second;

                for (auto orderIt = ordersAtPrice.begin();
                     orderIt != ordersAtPrice.end() && remainingQty > 0;
                     /* increment in loop */) {

                    auto& sellOrder = *orderIt;

                    // Skip cancelled orders
                    if (sellOrder->status == CANCELLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                        continue;
                    }

                    // Determine match quantity
                    int matchQty = min(remainingQty, sellOrder->getRemainingQuantity());

                    // Execute the trade
                    auto trade = make_shared<Trade>(order->id, sellOrder->id, order->symbol, matchPrice, matchQty);
                    tradeHistory.push_back(trade);

                    // Update quantities
                    remainingQty -= matchQty;
                    order->filled_quantity += matchQty;
                    sellOrder->filled_quantity += matchQty;

                    // Update order status
                    updateOrderStatus(sellOrder);

                    cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                         << " at $" << fixed << setprecision(2) << matchPrice
                         << " (Buy: " << order->id << " [FOK], Sell: " << sellOrder->id << ")" << endl;

                    // Remove filled sell orders
                    if (sellOrder->status == FILLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                    } else {
                        ++orderIt;
                    }
                }

                // Clean up empty price levels
                if (ordersAtPrice.empty()) {
                    sellIt = sellBook.erase(sellIt);
                } else {
                    ++sellIt;
                }
            }
        } else { // SELL FOK order
            auto& buyBook = buyOrders[order->symbol];
            int remainingQty = order->quantity;

            // Go through buy orders from highest to lowest price
            for (auto buyIt = buyBook.begin();
                 buyIt != buyBook.end() && remainingQty > 0 && buyIt->first >= order->price;
                 /* increment in loop */) {

                double matchPrice = buyIt->first;
                auto& ordersAtPrice = buyIt->second;

                for (auto orderIt = ordersAtPrice.begin();
                     orderIt != ordersAtPrice.end() && remainingQty > 0;
                     /* increment in loop */) {

                    auto& buyOrder = *orderIt;

                    // Skip cancelled orders
                    if (buyOrder->status == CANCELLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                        continue;
                    }

                    // Determine match quantity
                    int matchQty = min(remainingQty, buyOrder->getRemainingQuantity());

                    // Execute the trade
                    auto trade = make_shared<Trade>(buyOrder->id, order->id, order->symbol, matchPrice, matchQty);
                    tradeHistory.push_back(trade);

                    // Update quantities
                    remainingQty -= matchQty;
                    order->filled_quantity += matchQty;
                    buyOrder->filled_quantity += matchQty;

                    // Update order status
                    updateOrderStatus(buyOrder);

                    cout << "\nTrade Executed: " << matchQty << " " << order->symbol
                         << " at $" << fixed << setprecision(2) << matchPrice
                         << " (Buy: " << buyOrder->id << ", Sell: " << order->id << " [FOK])" << endl;

                    // Remove filled buy orders
                    if (buyOrder->status == FILLED) {
                        orderIt = ordersAtPrice.erase(orderIt);
                    } else {
                        ++orderIt;
                    }
                }

                // Clean up empty price levels
                if (ordersAtPrice.empty()) {
                    buyIt = buyBook.erase(buyIt);
                } else {
                    ++buyIt;
                }
            }
        }

        // Update FOK order status
        updateOrderStatus(order);

        // Should be completely filled
        return order->status == FILLED;
    }

    void updateOrderStatus(shared_ptr<Order>& order) {
        if (order->filled_quantity >= order->quantity) {
            order->status = FILLED;
        } else if (order->filled_quantity > 0) {
            order->status = PARTIALLY_FILLED;
        }
    }

    shared_mutex& getOrCreateSymbolMutex(const string& symbol) {
        static mutex createMutex;

        // First check without locking for performance
        auto it = symbolMutexes.find(symbol);
        if (it != symbolMutexes.end()) {
            return it->second;
        }

        // If not found, lock and check again (double-checked locking pattern)
        {
            lock_guard<mutex> lock(createMutex);
            // Check again after acquiring lock
            it = symbolMutexes.find(symbol);
            if (it != symbolMutexes.end()) {
                return it->second;
            }
            // Create new mutex if still needed
            return symbolMutexes[symbol];
        }
    }
};

int main(int argc, char* argv[]) {
    OrderBook orderBook;

    cout << "Starting Stock Market Order Matching System with Circuit Breakers..." << endl;

    // Set up stock-specific price bands (example)
    orderBook.setStockPriceBand("RELIANCE", 2000.0, 5.0);  // 5% band
    orderBook.setStockPriceBand("INFY", 1500.0, 10.0);     // 10% band
    orderBook.setStockPriceBand("TATASTEEL", 800.0, 20.0); // 20% band

    // Check if we're running from a command file
    if (argc > 1) {
        ifstream commandFile(argv[1]);
        string line;

        if (!commandFile.is_open()) {
            cerr << "Failed to open command file: " << argv[1] << endl;
            return 1;
        }

        while (getline(commandFile, line)) {
            istringstream iss(line);
            string command;

            iss >> command;

            if (command == "exit") {
                break;
            } else if (command == "place_order") {
                string typeStr, variantStr, symbolStr;
                double price;
                int quantity;

                iss >> typeStr >> variantStr >> price >> quantity >> symbolStr;

                OrderType type = (typeStr == "BUY") ? BUY : SELL;
                OrderVariant variant;

                if (variantStr == "LIMIT") variant = LIMIT;
                else if (variantStr == "MARKET") variant = MARKET;
                else if (variantStr == "IOC") variant = IOC;
                else if (variantStr == "FOK") variant = FOK;
                else {
                    cerr << "Invalid order variant: " << variantStr << endl;
                    continue;
                }

                orderBook.placeOrder(type, variant, price, quantity, symbolStr);
            } else if (command == "cancel_order") {
                int orderId;
                iss >> orderId;
                orderBook.cancelOrder(orderId);
            } else if (command == "print_orderbook") {
                string symbol;
                iss >> symbol;
                orderBook.printOrderBook(symbol);
            } else if (command == "print_trades") {
                string symbol;
                iss >> symbol;
                orderBook.printTradeHistory(symbol);
            } else if (command == "update_index") {
                double indexValue;
                iss >> indexValue;
                orderBook.updateIndexValue(indexValue, time(nullptr));
            } else {
                cerr << "Unknown command: " << command << endl;
            }
        }

        commandFile.close();
        return 0;
    }

    // If no command file is provided, run the default test cases

    // Test case 1: Simple matching with limit orders
    cout << "\n===== Test Case 1: Basic Matching with Limit Orders =====" << endl;
    orderBook.placeOrder(BUY, LIMIT, 100.50, 10, "AAPL");
    orderBook.placeOrder(BUY, LIMIT, 101.00, 5, "AAPL");
    orderBook.placeOrder(SELL, LIMIT, 100.00, 8, "AAPL");
    orderBook.printOrderBook("AAPL");
    orderBook.printTradeHistory("AAPL");

    // Test case 2: Market Order
    cout << "\n===== Test Case 2: Market Order =====" << endl;
    // Place some limit orders to create liquidity
    orderBook.placeOrder(BUY, LIMIT, 25.00, 5, "MSFT");
    orderBook.placeOrder(BUY, LIMIT, 24.75, 10, "MSFT");
    orderBook.placeOrder(SELL, LIMIT, 25.50, 5, "MSFT");
    orderBook.placeOrder(SELL, LIMIT, 26.00, 10, "MSFT");
    cout << "\nBefore Market Order:" << endl;
    orderBook.printOrderBook("MSFT");

    // Place a market buy order
    orderBook.placeOrder(BUY, MARKET, 0.0, 7, "MSFT");
    cout << "\nAfter Market Order:" << endl;
    orderBook.printOrderBook("MSFT");
    orderBook.printTradeHistory("MSFT");

    // Test case 3: IOC Order (Immediate or Cancel)
    cout << "\n===== Test Case 3: IOC Order =====" << endl;
    // Place some limit orders first
    orderBook.placeOrder(BUY, LIMIT, 50.00, 5, "GOOG");
    orderBook.placeOrder(SELL, LIMIT, 51.00, 10, "GOOG");
    cout << "\nBefore IOC Order:" << endl;
    orderBook.printOrderBook("GOOG");

    // Place an IOC sell order that crosses with the buy
    orderBook.placeOrder(SELL, IOC, 50.00, 7, "GOOG");
    cout << "\nAfter IOC Order:" << endl;
    orderBook.printOrderBook("GOOG");
    orderBook.printTradeHistory("GOOG");

    // Test case 4: FOK Order (Fill or Kill)
    cout << "\n===== Test Case 4: FOK Order =====" << endl;
    // Place some limit orders first
    orderBook.placeOrder(BUY, LIMIT, 150.00, 5, "AMZN");
    orderBook.placeOrder(SELL, LIMIT, 151.00, 5, "AMZN");
    orderBook.placeOrder(SELL, LIMIT, 152.00, 5, "AMZN");
    cout << "\nBefore FOK Orders:" << endl;
    orderBook.printOrderBook("AMZN");

    // Place FOK buy order that can be fully filled
    orderBook.placeOrder(BUY, FOK, 151.00, 5, "AMZN");

    // Place FOK buy order that cannot be fully filled
    orderBook.placeOrder(BUY, FOK, 151.00, 10, "AMZN");

    cout << "\nAfter FOK Orders:" << endl;
    orderBook.printOrderBook("AMZN");
    orderBook.printTradeHistory("AMZN");

    // Test case 5: Stock-specific price band
    cout << "\n===== Test Case 5: Stock-Specific Price Band =====" << endl;
    // Try to place order outside of price band
    orderBook.placeOrder(BUY, LIMIT, 2200.0, 10, "RELIANCE"); // Above upper limit (2100.0)
    orderBook.placeOrder(SELL, LIMIT, 1850.0, 10, "RELIANCE"); // Below lower limit (1900.0)
    // Valid order within band
    orderBook.placeOrder(BUY, LIMIT, 2050.0, 10, "RELIANCE");
    orderBook.printOrderBook("RELIANCE");

    // Test case 6: Circuit breaker simulation
    cout << "\n===== Test Case 6: Market-Wide Circuit Breaker =====" << endl;
    cout << "Simulating 12% market drop at 11:30 AM..." << endl;

    // Create a time for 11:30 AM
    time_t currentTime = time(nullptr);
    struct tm* timeinfo = localtime(&currentTime);
    timeinfo->tm_hour = 11;
    timeinfo->tm_min = 30;
    time_t simulatedTime = mktime(timeinfo);

    // Trigger level 1 circuit breaker (12% drop from reference)
    orderBook.updateIndexValue(15400.0, simulatedTime); // ~12% drop from 17500

    // Try placing an order during circuit halt
    orderBook.placeOrder(BUY, MARKET, 0.0, 5, "INFY");

    // Simulate time passage (advance by 50 minutes - after halt ends)
    simulatedTime += 50 * 60; // 50 minutes
    orderBook.updateIndexValue(15400.0, simulatedTime);

    cout << "\nTesting after pre-open auction ends..." << endl;
    // Advance by 20 more minutes (past pre-open auction)
    simulatedTime += 20 * 60;
    orderBook.updateIndexValue(15400.0, simulatedTime);

    // Now we should be able to place orders again
    orderBook.placeOrder(BUY, LIMIT, 1520.0, 5, "INFY");
    orderBook.printOrderBook("INFY");

    return 0;
}