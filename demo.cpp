#include <iostream>
#include <string>

class SimpleTrader {
private:
    std::string symbol;
    double balance;

public:
    SimpleTrader(std::string sym, double bal) : symbol(sym), balance(bal) {}

    void show_info() {
        std::cout << "Trading " << symbol << " with balance: $" << balance << std::endl;
    }

    double calculate_pnl(double buy_price, double sell_price, int quantity) {
        return (sell_price - buy_price) * quantity;
    }
};

int main() {
    SimpleTrader trader("AAPL", 10000.0);
    trader.show_info();

    double pnl = trader.calculate_pnl(150.0, 155.0, 50);
    std::cout << "PnL: $" << pnl << std::endl;

    std::cout << "Hello, C++ Trading!" << std::endl;
    return 0;
}