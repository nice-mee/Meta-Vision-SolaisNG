#!/usr/bin/env python
import rclpy

from solais_interfaces import GimbalPose
from communicator import UARTCommunicator
import config

def callback(data):
    yaw = data.data[0]
    pitch = data.data[1]

    rospy.wait_for_service('detection_tracking_service')
    try:
        detection_tracking = rospy.ServiceProxy('detection_tracking_service', DetectionTracking)
        response = detection_tracking(yaw, pitch)

        uart = UARTCommunicator(config)

        uart.process_one_packet(config.SEARCH_TARGET, response.new_yaw, response.new_pitch)
    except rospy.ServiceException as e:
        print("Service call failed: %s" % e)

def main():
    
    rospy.init_node('yaw_pitch_listener', anonymous=True)
    rospy.Subscriber('/gimbal_pose', Float64MultiArray, callback)
    rospy.spin()

if __name__ == '__main__':
    main()