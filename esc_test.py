from gpiozero import PWMOutputDevice
from gpiozero.pins.pigpio import PiGPIOFactory
from time import sleep

factory = PiGPIOFactory()  # hardware-timed PWM via pigpiod

esc1 = PWMOutputDevice(26, frequency=50, pin_factory=factory)
esc2 = PWMOutputDevice(13, frequency=50, pin_factory=factory)

MIN = 0.05   # 1ms pulse - minimum throttle / arm signal
MAX = 0.10   # 2ms pulse - full throttle

def set_speed(esc, pct):
    """pct: 0-100, mapped between MIN and MAX pulse width"""
    esc.value = MIN + (MAX - MIN) * (pct / 100)

try:
    print("Arming both ESCs at minimum throttle...")
    esc1.value = MIN
    esc2.value = MIN
    sleep(3)
    print("Should be armed now - listen for the ESC beep/tone on each.")
    input("Press Enter only once both motors are confirmed idle/quiet...")

    for pct in [5, 10, 15, 20]:
        print(f"Setting both motors to {pct}%")
        set_speed(esc1, pct)
        set_speed(esc2, pct)
        sleep(2)

    print("Test step complete. Ctrl+C to stop, or let it fall through to cleanup.")
    sleep(3)

except KeyboardInterrupt:
    print("Interrupted by user.")
finally:
    esc1.value = 0
    esc2.value = 0
    print("Both motors stopped, GPIO cleaned up.")
