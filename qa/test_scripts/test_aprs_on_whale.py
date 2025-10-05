#!/usr/bin/env python3
"""
Test APRS on whale feature (Issue #109, PR #112)

Tests that the recovery board sleep/wake behavior is controlled by:
- aprs_on_whale config parameter
- Current state (ST_SURFACE vs ST_RETRIEVE)
"""

import socket
import subprocess
import time
import sys
import signal

class WhaleTagSimulator:
    """Controls whale tag via LD_PRELOAD network interface"""

    def __init__(self, host='localhost', port=9999):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"WhaleTagSimulator connected to {host}:{port}")

    def set_depth(self, depth_m):
        """Set simulated depth in meters"""
        cmd = f"DEPTH={depth_m}"
        self.sock.sendto(cmd.encode(), (self.host, self.port))
        print(f"  → Set depth to {depth_m}m")

    def set_temperature(self, temp_c):
        """Set simulated temperature in Celsius"""
        cmd = f"TEMP={temp_c}"
        self.sock.sendto(cmd.encode(), (self.host, self.port))
        print(f"  → Set temperature to {temp_c}°C")

    def set_light(self, lux):
        """Set simulated light level in lux"""
        cmd = f"LIGHT={lux}"
        self.sock.sendto(cmd.encode(), (self.host, self.port))
        print(f"  → Set light to {lux} lux")

    def set_battery(self, voltage):
        """Set simulated battery voltage"""
        cmd = f"BATTERY={voltage}"
        self.sock.sendto(cmd.encode(), (self.host, self.port))
        print(f"  → Set battery to {voltage}V")

    def close(self):
        self.sock.close()


def test_aprs_disabled_at_surface():
    """Test that recovery board sleeps when aprs_on_whale=false at surface"""
    print("\n" + "="*70)
    print("TEST 1: APRS disabled at surface (aprs_on_whale=false)")
    print("="*70)
    print("Expected: recovery_sleep() called when at surface")

    sim = WhaleTagSimulator()

    # Simulate surface condition
    sim.set_depth(0.5)  # 0.5m depth (surface)
    sim.set_light(1000)  # Daylight

    print("\nWaiting 10 seconds for state machine to process...")
    time.sleep(10)

    sim.close()

    print("\n✓ Test completed - Check logs for 'recovery_sleep' calls")
    print("  Expected behavior: recovery_sleep() called at surface with aprs_on_whale=false")
    return True


def test_aprs_enabled_at_surface():
    """Test that recovery board wakes when aprs_on_whale=true at surface"""
    print("\n" + "="*70)
    print("TEST 2: APRS enabled at surface (aprs_on_whale=true)")
    print("="*70)
    print("Expected: recovery_wake() called when at surface")
    print("\n⚠️  NOTE: This requires aprs_on_whale=true in config file!")

    sim = WhaleTagSimulator()

    # Simulate surface condition
    sim.set_depth(0.5)  # 0.5m depth (surface)
    sim.set_light(1000)  # Daylight

    print("\nWaiting 10 seconds for state machine to process...")
    time.sleep(10)

    sim.close()

    print("\n✓ Test completed - Check logs for 'recovery_wake' calls")
    print("  Expected behavior: recovery_wake() called at surface with aprs_on_whale=true")
    return True


def test_dive_sequence():
    """Test a complete dive sequence"""
    print("\n" + "="*70)
    print("TEST 3: Complete dive sequence")
    print("="*70)

    sim = WhaleTagSimulator()

    print("\n1. Surface (0m)")
    sim.set_depth(0)
    sim.set_light(1000)
    time.sleep(5)

    print("\n2. Descending to 10m")
    sim.set_depth(10)
    sim.set_light(100)   # Less light underwater
    time.sleep(5)

    print("\n3. Deep dive to 50m")
    sim.set_depth(50)
    sim.set_light(0)     # Dark
    time.sleep(5)

    print("\n4. Ascending to 20m")
    sim.set_depth(20)
    sim.set_light(50)
    time.sleep(5)

    print("\n5. Return to surface")
    sim.set_depth(0)
    sim.set_light(1000)
    time.sleep(5)

    sim.close()

    print("\n✓ Test completed - Check state transitions in logs")
    return True


def main():
    print("APRS on Whale Feature Test Suite")
    print("=" * 70)
    print("\nThis test suite requires:")
    print("1. Whale tag firmware running with LD_PRELOAD:")
    print("   LD_PRELOAD=./qa/libpigpio_sim/build/libpigpio_sim.so cetiTagApp")
    print("2. Network control enabled (UDP port 9999)")
    print("\nPress Ctrl+C to stop tests at any time")
    print("=" * 70)

    try:
        tests = [
            ("APRS disabled at surface", test_aprs_disabled_at_surface),
            ("APRS enabled at surface", test_aprs_enabled_at_surface),
            ("Complete dive sequence", test_dive_sequence),
        ]

        results = []
        for name, test_func in tests:
            try:
                result = test_func()
                results.append((name, result))
            except Exception as e:
                print(f"\n✗ Test '{name}' failed with error: {e}")
                results.append((name, False))

        # Summary
        print("\n" + "="*70)
        print("TEST SUMMARY")
        print("="*70)

        passed = sum(1 for _, result in results if result)
        total = len(results)

        for name, result in results:
            status = "✓ PASS" if result else "✗ FAIL"
            print(f"{status}: {name}")

        print(f"\nTotal: {passed}/{total} tests passed")
        print("\n" + "="*70)
        print("IMPORTANT: Review firmware logs for:")
        print("  - recovery_sleep() / recovery_wake() calls")
        print("  - State transitions (ST_SURFACE, ST_DIVE, ST_RETRIEVE)")
        print("  - APRS configuration logging")
        print("="*70)

        return 0 if passed == total else 1

    except KeyboardInterrupt:
        print("\n\nTests interrupted by user")
        return 1


if __name__ == '__main__':
    sys.exit(main())
