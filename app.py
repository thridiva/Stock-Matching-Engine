from flask import Flask, render_template, request, redirect, url_for, jsonify
import subprocess
import os
import json
import tempfile
import time

app = Flask(__name__)

# Store the output of the orderbook program
order_book_data = {
    "order_book": {},
    "trade_history": {}
}

# Store command history for persistence between requests
command_history = []

# Initialize with stock-specific price bands
command_history.append("setStockPriceBand RELIANCE 2000.0 5.0")
command_history.append("setStockPriceBand INFY 1500.0 10.0")
command_history.append("setStockPriceBand TATASTEEL 800.0 20.0")


@app.route('/')
def index():
    return render_template('index.html')


@app.route('/place_order', methods=['POST'])
def place_order():
    # Get order details from form
    order_type = request.form['order_type']  # BUY or SELL
    order_variant = request.form['order_variant']  # LIMIT, MARKET, IOC, FOK
    price = request.form['price'] if 'price' in request.form and request.form['price'] else "0.0"
    quantity = request.form['quantity']
    symbol = request.form['symbol']

    # Add this order to the command history
    command_history.append(f"place_order {order_type} {order_variant} {price} {quantity} {symbol}")

    # Create a temporary file to store commands for the C++ program
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp:
        # Write all previous commands to rebuild the state
        for cmd in command_history:
            tmp.write(cmd + "\n")

        # Write commands to view the results
        tmp.write("print_orderbook " + symbol + "\n")
        tmp.write("print_trades " + symbol + "\n")
        tmp.write("exit\n")

        tmp_filename = tmp.name

    try:
        # Run the C++ executable with the command file
        result = subprocess.run(
            ['./cpp_src/orderbook', tmp_filename],
            capture_output=True, text=True
        )

        # Parse the output
        parse_orderbook_output(result.stdout)

        # Clean up the temporary file
        os.unlink(tmp_filename)

        # Redirect to the results page
        return redirect(url_for('results', symbol=symbol))
    except Exception as e:
        return f"Error: {str(e)}"


@app.route('/view_orderbook', methods=['POST'])
def view_orderbook():
    symbol = request.form['view_symbol']
    if not symbol:
        return redirect(url_for('index'))

    # Create a temporary file with commands
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp:
        # Write all previous commands to rebuild the state
        for cmd in command_history:
            tmp.write(cmd + "\n")

        # Write commands to view the results
        tmp.write("print_orderbook " + symbol + "\n")
        tmp.write("print_trades " + symbol + "\n")
        tmp.write("exit\n")

        tmp_filename = tmp.name

    try:
        # Run the C++ executable with the command file
        result = subprocess.run(
            ['./cpp_src/orderbook', tmp_filename],
            capture_output=True, text=True
        )

        # Parse the output
        parse_orderbook_output(result.stdout)

        # Clean up the temporary file
        os.unlink(tmp_filename)

        # Redirect to the results page
        return redirect(url_for('results', symbol=symbol))
    except Exception as e:
        return f"Error: {str(e)}"


@app.route('/reset_orderbook', methods=['POST'])
def reset_orderbook():
    # Reset command history to only include price bands
    global command_history
    command_history = [
        "setStockPriceBand RELIANCE 2000.0 5.0",
        "setStockPriceBand INFY 1500.0 10.0",
        "setStockPriceBand TATASTEEL 800.0 20.0"
    ]

    # Clear stored data
    order_book_data["order_book"] = {}
    order_book_data["trade_history"] = {}

    return redirect(url_for('index'))


@app.route('/results')
def results():
    symbol = request.args.get('symbol', '')
    return render_template('results.html', symbol=symbol)


@app.route('/orderbook/<symbol>')
def orderbook(symbol):
    return render_template('orderbook.html', symbol=symbol)


@app.route('/trades/<symbol>')
def trades(symbol):
    return render_template('trades.html', symbol=symbol)


@app.route('/api/orderbook/<symbol>')
def api_orderbook(symbol):
    if symbol in order_book_data["order_book"]:
        return jsonify(order_book_data["order_book"][symbol])
    return jsonify({"buy_orders": [], "sell_orders": []})


@app.route('/api/trades/<symbol>')
def api_trades(symbol):
    if symbol in order_book_data["trade_history"]:
        return jsonify(order_book_data["trade_history"][symbol])
    return jsonify([])


@app.route('/api/symbols')
def api_symbols():
    # Get all unique symbols from order book data
    symbols = set()
    for symbol in order_book_data["order_book"].keys():
        symbols.add(symbol)
    return jsonify(list(symbols))


def parse_orderbook_output(output):
    """Parse the output from the C++ program and store it in the global order_book_data."""
    lines = output.split('\n')

    current_section = None
    current_symbol = None

    buy_orders = []
    sell_orders = []
    trades = []

    for line in lines:
        line = line.strip()

        if not line:
            continue

        if line.startswith("Order Book for "):
            current_section = "orderbook"
            current_symbol = line.split("Order Book for ")[1].strip(":")
            buy_orders = []
            sell_orders = []
            order_book_data["order_book"][current_symbol] = {
                "buy_orders": buy_orders,
                "sell_orders": sell_orders
            }

        elif line.startswith("Trade History for "):
            current_section = "trades"
            current_symbol = line.split("Trade History for ")[1].strip(":")
            trades = []
            order_book_data["trade_history"][current_symbol] = trades

        elif current_section == "orderbook" and line.startswith("Price: $"):
            # Parse order book line
            parts = line.split(", ")
            order = {}
            for part in parts:
                if part.startswith("Price: $"):
                    order["price"] = float(part.split("Price: $")[1])
                elif part.startswith("Qty: "):
                    order["quantity"] = int(part.split("Qty: ")[1])
                elif part.startswith("ID: "):
                    order["id"] = int(part.split("ID: ")[1])
                elif part.startswith("Type: "):
                    order["type"] = part.split("Type: ")[1]
                elif part.startswith("Status: "):
                    order["status"] = part.split("Status: ")[1]
                elif part.startswith("Time: "):
                    order["timestamp"] = part.split("Time: ")[1]

            # Add to the appropriate list based on whether it's in Buy Orders or Sell Orders section
            if "Buy Orders" in output and "Sell Orders" in output:
                buy_sell_split = output.split("Buy Orders")[1].split("Sell Orders")
                if len(buy_sell_split) > 0 and line in buy_sell_split[0]:
                    buy_orders.append(order)
                else:
                    sell_orders.append(order)
            else:
                # Fallback if we can't determine which section it belongs to
                if current_symbol in order_book_data["order_book"]:
                    order_book_data["order_book"][current_symbol]["buy_orders"].append(order)

        elif current_section == "trades" and line.startswith("Time: "):
            # Parse trade history line
            parts = line.split(", ")
            trade = {}
            for part in parts:
                if part.startswith("Time: "):
                    trade["timestamp"] = part.split("Time: ")[1]
                elif part.startswith("Qty: "):
                    trade["quantity"] = int(part.split("Qty: ")[1])
                elif part.startswith("Price: $"):
                    trade["price"] = float(part.split("Price: $")[1])
                elif part.startswith("Buy ID: "):
                    trade["buy_order_id"] = int(part.split("Buy ID: ")[1])
                elif part.startswith("Sell ID: "):
                    trade["sell_order_id"] = int(part.split("Sell ID: ")[1])

            trades.append(trade)


if __name__ == '__main__':
    app.run(debug=True)