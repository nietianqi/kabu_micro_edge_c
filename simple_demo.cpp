#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// 简单的交易机器人演示
class SimpleTrader {
private:
    std::string symbol;
    double balance;
    std::vector<double> prices;

public:
    SimpleTrader(std::string sym, double initial_balance)
        : symbol(sym), balance(initial_balance) {}

    void add_price(double price) {
        prices.push_back(price);
        std::cout << "📈 " << symbol << " 价格更新: ¥" << price << std::endl;
    }

    void check_signal() {
        if (prices.size() < 2) return;

        double current = prices.back();
        double previous = prices[prices.size() - 2];

        if (current > previous * 1.01) {  // 价格上涨 1%
            std::cout << "🚀 买入信号! " << symbol << " 价格上涨" << std::endl;
        } else if (current < previous * 0.99) {  // 价格下跌 1%
            std::cout << "📉 卖出信号! " << symbol << " 价格下跌" << std::endl;
        }
    }

    void show_balance() {
        std::cout << "💰 当前余额: ¥" << balance << std::endl;
    }
};

int main() {
    std::cout << "🎯 简单交易机器人演示" << std::endl;
    std::cout << "=================================" << std::endl;

    // 创建交易机器人
    SimpleTrader trader("7269", 100000.0);

    // 模拟价格数据
    std::vector<double> sample_prices = {500.0, 502.5, 498.0, 505.0, 503.5, 510.0};

    for (double price : sample_prices) {
        trader.add_price(price);
        trader.check_signal();

        // 模拟延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    trader.show_balance();

    std::cout << "✅ 演示完成!" << std::endl;
    return 0;
}