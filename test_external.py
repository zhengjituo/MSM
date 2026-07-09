#!/usr/bin/env python3
"""
MSM 外部测试客户端
使用 Python 向运行中的 adder_service 发送消息
"""

import sys
import time

def print_usage():
    print("MSM External Test Client")
    print("========================")
    print("Commands:")
    print("  set_a <value>  - Set variable A")
    print("  set_b <value>  - Set variable B")
    print("  trigger        - Trigger ADD action")
    print("  get_sum        - Get result")
    print("  quit           - Exit")
    print("")

def main():
    print_usage()
    print("Note: This requires adder_service running separately.")
    print("Usage:")
    print("  1. Start adder_service in one terminal")
    print("  2. Run this script in another terminal")
    print("")

if __name__ == "__main__":
    main()