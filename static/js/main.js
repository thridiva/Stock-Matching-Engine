// Wait for the DOM to be fully loaded
document.addEventListener('DOMContentLoaded', function() {
    // Handle the order variant change to toggle price field visibility
    const orderVariantSelect = document.getElementById('order_variant');
    const priceGroup = document.getElementById('price-group');

    if (orderVariantSelect && priceGroup) {
        orderVariantSelect.addEventListener('change', function() {
            if (this.value === 'MARKET') {
                priceGroup.style.display = 'none';
                document.getElementById('price').removeAttribute('required');
            } else {
                priceGroup.style.display = 'block';
                document.getElementById('price').setAttribute('required', 'required');
            }
        });
    }

    // Handle the view order book button
    const viewBtn = document.getElementById('view-btn');
    if (viewBtn) {
        viewBtn.addEventListener('click', function() {
            const symbol = document.getElementById('view-symbol').value;
            if (symbol) {
                window.location.href = `/orderbook/${symbol}`;
            } else {
                alert('Please enter a symbol');
            }
        });
    }

    // Handle tab switching on results page
    const tabs = document.querySelectorAll('.tab');
    if (tabs.length > 0) {
        tabs.forEach(tab => {
            tab.addEventListener('click', function() {
                // Remove active class from all tabs
                document.querySelectorAll('.tab').forEach(t => {
                    t.classList.remove('active');
                });

                // Remove active class from all tab panes
                document.querySelectorAll('.tab-pane').forEach(p => {
                    p.classList.remove('active');
                });

                // Add active class to clicked tab
                this.classList.add('active');

                // Add active class to corresponding tab pane
                const tabId = this.getAttribute('data-tab');
                document.getElementById(tabId).classList.add('active');
            });
        });
    }

    // Load data if we're on a page with symbol
    if (typeof symbol !== 'undefined') {
        // Load order book data
        if (document.getElementById('buy-orders-table') && document.getElementById('sell-orders-table')) {
            loadOrderBook(symbol);
        }

        // Load trade history data
        if (document.getElementById('trades-table')) {
            loadTradeHistory(symbol);
        }
    }
});

// Function to load order book data
function loadOrderBook(symbol) {
    fetch(`/api/orderbook/${symbol}`)
        .then(response => response.json())
        .then(data => {
            // Update buy orders table
            const buyOrdersTable = document.getElementById('buy-orders-table').querySelector('tbody');
            buyOrdersTable.innerHTML = '';

            if (data.buy_orders && data.buy_orders.length > 0) {
                data.buy_orders.forEach(order => {
                    const row = document.createElement('tr');
                    row.innerHTML = `
                        <td>${order.price.toFixed(2)}</td>
                        <td>${order.quantity}</td>
                        <td>${order.type}</td>
                        <td>${order.status}</td>
                        <td>${order.timestamp}</td>
                    `;
                    buyOrdersTable.appendChild(row);
                });
            } else {
                const row = document.createElement('tr');
                row.innerHTML = '<td colspan="5">No buy orders</td>';
                buyOrdersTable.appendChild(row);
            }

            // Update sell orders table
            const sellOrdersTable = document.getElementById('sell-orders-table').querySelector('tbody');
            sellOrdersTable.innerHTML = '';

            if (data.sell_orders && data.sell_orders.length > 0) {
                data.sell_orders.forEach(order => {
                    const row = document.createElement('tr');
                    row.innerHTML = `
                        <td>${order.price.toFixed(2)}</td>
                        <td>${order.quantity}</td>
                        <td>${order.type}</td>
                        <td>${order.status}</td>
                        <td>${order.timestamp}</td>
                    `;
                    sellOrdersTable.appendChild(row);
                });
            } else {
                const row = document.createElement('tr');
                row.innerHTML = '<td colspan="5">No sell orders</td>';
                sellOrdersTable.appendChild(row);
            }

            // Create depth chart if Chart.js is available
            if (typeof Chart !== 'undefined' && document.getElementById('depth-chart')) {
                createDepthChart(data);
            }
        })
        .catch(error => {
            console.error('Error loading order book:', error);
        });
}

// Function to load trade history
function loadTradeHistory(symbol) {
    fetch(`/api/trades/${symbol}`)
        .then(response => response.json())
        .then(data => {
            const tradesTable = document.getElementById('trades-table').querySelector('tbody');
            tradesTable.innerHTML = '';

            if (data && data.length > 0) {
                data.forEach(trade => {
                    const row = document.createElement('tr');
                    row.innerHTML = `
                        <td>${trade.timestamp}</td>
                        <td>${trade.price.toFixed(2)}</td>
                        <td>${trade.quantity}</td>
                        <td>${trade.buy_order_id}</td>
                        <td>${trade.sell_order_id}</td>
                    `;
                    tradesTable.appendChild(row);
                });

                // Create price chart if Chart.js is available
                if (typeof Chart !== 'undefined' && document.getElementById('price-chart')) {
                    createPriceChart(data);
                }
            } else {
                const row = document.createElement('tr');
                row.innerHTML = '<td colspan="5">No trades</td>';
                tradesTable.appendChild(row);
            }
        })
        .catch(error => {
            console.error('Error loading trade history:', error);
        });
}

// Function to create depth chart
function createDepthChart(data) {
    // Get the canvas element
    const ctx = document.getElementById('depth-chart').getContext('2d');

    // Prepare data for chart
    const buyPrices = data.buy_orders.map(order => order.price);
    const buyQuantities = data.buy_orders.map(order => order.quantity);
    const sellPrices = data.sell_orders.map(order => order.price);
    const sellQuantities = data.sell_orders.map(order => order.quantity);

    // Create chart
    new Chart(ctx, {
        type: 'bar',
        data: {
            labels: [...buyPrices, ...sellPrices],
            datasets: [
                {
                    label: 'Buy Orders',
                    data: [...buyQuantities, ...Array(sellPrices.length).fill(0)],
                    backgroundColor: 'rgba(75, 192, 192, 0.6)',
                    borderColor: 'rgba(75, 192, 192, 1)',
                    borderWidth: 1
                },
                {
                    label: 'Sell Orders',
                    data: [...Array(buyPrices.length).fill(0), ...sellQuantities],
                    backgroundColor: 'rgba(255, 99, 132, 0.6)',
                    borderColor: 'rgba(255, 99, 132, 1)',
                    borderWidth: 1
                }
            ]
        },
        options: {
            responsive: true,
            scales: {
                x: {
                    title: {
                        display: true,
                        text: 'Price'
                    }
                },
                y: {
                    title: {
                        display: true,
                        text: 'Quantity'
                    },
                    beginAtZero: true
                }
            }
        }
    });
}

// Function to create price chart for trades
function createPriceChart(data) {
    // Get the canvas element
    const ctx = document.getElementById('price-chart').getContext('2d');

    // Prepare data for chart
    const timestamps = data.map(trade => trade.timestamp);
    const prices = data.map(trade => trade.price);

    // Create chart
    new Chart(ctx, {
        type: 'line',
        data: {
            labels: timestamps,
            datasets: [{
                label: 'Trade Price',
                data: prices,
                borderColor: 'rgba(54, 162, 235, 1)',
                backgroundColor: 'rgba(54, 162, 235, 0.2)',
                borderWidth: 2,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            scales: {
                x: {
                    title: {
                        display: true,
                        text: 'Time'
                    }
                },
                y: {
                    title: {
                        display: true,
                        text: 'Price'
                    }
                }
            }
        }
    });
}