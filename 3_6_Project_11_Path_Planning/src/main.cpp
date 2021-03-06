/**
 * Project 11 of Udacity's Self-Driving Car Nanodegree Program - Path Planning
 * The wask is to plan the trajectory of a car on a highway and to steer the vehicle along the lanes, handle overtaking
 * maneuvers within a traffic jam and control speed with help of the data of the surround cars.
 *
 * Copyright (c) 2018 by Michael Ikemann - https://alyxion.github.io
 *
 * Original code Copyright (c) Udacity - https://www.Udacity.com
 *
 */

#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = fabs(theta-heading);
  angle = min(2*pi() - angle, angle);

  if(angle > pi()/4)
  {
    closestWaypoint++;
  if (closestWaypoint == maps_x.size())
  {
    closestWaypoint = 0;
  }
  }

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

// ---------------------------------------------------------------------------------------------------------------------

/** @brief Enumerates the indices of the sensor fusion states */
enum SensorFusionProperties
{
	VehicleX = 3,
	VehicleY = 4,
	VehicleD = 6,
	VehicleS = 5
};

// ---------------------------------------------------------------------------------------------------------------------

/** @brief The path planner class receives the data of a highway track, the data of a car and data of surrounding cars
 * and shall calculate the movement direction, decide when to change lanes and the vehicle's precise trajectory to do
 * so.
 */
class PathPlanner
{
protected:
	vector<double> *mWayPointsX = nullptr;	///< Holds the track's x coordinates
	vector<double> *mWayPointsY = nullptr;	///< Holds the track's y coordinates
	vector<double> *mWayPointsS = nullptr;	///< Holds the track's s cooordinates (an increasing number actually)
	vector<double> *mWayPointsDx = nullptr;	///< Holds the track's normal vectors at given point
	vector<double> *mWayPointsDy = nullptr;	///< Holds the track's normal vectors at given point

	double mFrequency = 0.02;		///< The frequency (in ms) at which the simulator is triggered and the nodes handled
	int miLaneCount = 3;			///< Defines the count of lanes defined
	double mLaneWidth = 4.0;		///< Defines the full lane width in meters
	double mHalfLaneWidth = 2.0;	///< Defines the half lane width in meters
	double mMaxLaneOffset = 0.25;	///< Maximum offset of lane center
	double mdMaxLaneChangeCosts = 50;	///< Maximum costs up to a a lane change will be considered
	int miMaxPenaltyBonus = 1000;	///< Defines the amount of "time" required before a lane change to the right.
	double mdCriticalCosts = 100;	///< Costs to assign if a lane change is impossible / dangerous

	double mOptimalSpeed = 49.5;	///< Defines the current road's optimal speed
	double mMaxBrake = 0.224;		///< Defines the maximum deceleration per time step
	double mMaxAccelerate = 0.224;	///< Defines the maximum acceleration per time step
	double mSafetyBuffer = 20.0;	///< Defines the safety buffer in meters we require to change a lane

	int mPlanningPointCount = 50; 	///< Defines the count of points in the trajectory
	double mTargetOffset = 30.0;	///< Defines the target offset in vehicle direction in meters
	int miTargetLane = -1;			///< The current target lane. -1 = No lane switch planned
	int miLeftLanePenalty = 0;		///< A penalty generated when staying for too long in the left lane for no reason

	int mBaseNodeCount = 3;				///< Defines the count of nodes to be passed to the spline function
	double mBaseNodeStepping = 30.0;	///< Defines the distance in meters between each node passed to the spline function

	double mCarX = 0.0;			///< Cars x position
	double mCarY = 0.0;			///< Cars y position
	double mCarS = 0.0;			///< Car's position on the track (along the track) in meters
	double mCarD = 0.0;			///< Car's lane position, center of most left lane = 2.0 (lane width = 4 meters)
	double mCarYaw = 0.0;		///< Car's yaw angle (viewing direction)
	double mCarSpeed = 0.0;		///< Car's current speed in miles/hour
	double mEndPathS = 0.0;		///< The path's end s
	double mEndPathD = 0.0;		///< The path's end d

	vector<double> mPreviousPathX;	///< Contains the previous path's x points
	vector<double> mPreviousPathY;	///< Contains the previous path's y points
	int miPreviousSize = 0;

	vector<vector<double> >  mSensorFusionData;	///< Contains the sensor fusion data, so of all currently known vehicles on our road side

	double mTargetSpeed = 0.0;		///< Holds the current target speed


public:
	//! Constructor
	PathPlanner()
	{

	}

	//! Sets the current data. For details about the variable's content see member definitions above
	void SetFrameData(vector<double> *WayPointsX, vector<double> *WayPointsY, vector<double> *WayPointsS, vector<double> *WayPointsDx, vector<double> *WayPointsDy,
			double CarX, double CarY, double CarS, double CarD, double CarYaw, double CarSpeed, double EndPathS, double EndPathD,
			vector<double> &PreviousPathX, vector<double> &PreviousPathY, vector<vector<double> > &SensorFusionData
			)
	{
		// assign data elements 1:1
		mWayPointsX = WayPointsX;
		mWayPointsY = WayPointsY;
		mWayPointsS = WayPointsS;
		mWayPointsDx = WayPointsDx;
		mWayPointsDy = WayPointsDy;

		mCarX = CarX;
		mCarY = CarY;
		mCarS = CarS;
		mCarD = CarD;
		mCarYaw = CarYaw;
		mCarSpeed = CarSpeed;

		mEndPathS = EndPathS;
		mEndPathD = EndPathD;
		mPreviousPathX = PreviousPathX;
		mPreviousPathY = PreviousPathY;
		mSensorFusionData = SensorFusionData;

		miPreviousSize = mPreviousPathX.size();

		// update car's if required
		if(miPreviousSize>0)
		{
			mCarS = mEndPathS;
		}
	}

	//! Calculates the costs of each lane to decide where to continue driving
	/** @return A vector of costs for each lane. The lower the costs the more attractive the lane is */
	vector<double> CalculateLaneCosts()
	{
		double safetyDistance = CalculateSafetyDistance()*2;	// get the safety distance. for overtaking look two
																// times as far into the future instead of needing
																// to change lanes in the last possible moment.

		int currentLane = GetCurrentLane();

		vector<double> laneCosts;	// the costs of each lane

		for(int laneIndex=0; laneIndex<miLaneCount; ++laneIndex)
		{
			double costs = 0.0;

			// don't allow two lanes in one step
			if(abs(laneIndex-currentLane)>1)
			{
				costs += mdCriticalCosts;
				laneCosts.push_back(costs);
				continue;	// no need for further calculations if movement is critical
			}

			double laneCenter = laneIndex*mLaneWidth+mHalfLaneWidth;
			double laneDiff = mCarD - laneCenter;

			// the longer the car is on the left lanes, the more attractive right ones do become
			{
				int penaltyBonus = miLeftLanePenalty;
				if(penaltyBonus>miMaxPenaltyBonus)
				{
					penaltyBonus = miMaxPenaltyBonus;
				}
				costs += fabs(laneDiff*laneDiff/4)*((miMaxPenaltyBonus-penaltyBonus)/(double)miMaxPenaltyBonus);
				costs += (miLaneCount-1-laneIndex)*2;	// make right lanes preferred by default
			}


			double blockedByCarInFront = 0.0;		// penalty costs for a car in front of us on a lane, costs
													// increase up to safetyDistance (about 50m at full speed) if
													// a car is right in front of us
			bool blockedByCarAtSide = false;		// remembers if there is a car right next to us on any frame or
													// would likely hit us because it's much faster than our car

			// for all cars nearby...
			for(int i=0; i<mSensorFusionData.size(); ++i)
			{
				float d = mSensorFusionData[i][SensorFusionProperties::VehicleD];
				// is the car within this lane?
				if(d < (mHalfLaneWidth+mLaneWidth*laneIndex+mHalfLaneWidth) &&
				   d > (mHalfLaneWidth+mLaneWidth*laneIndex-mHalfLaneWidth) )
				{
					double vx = mSensorFusionData[i][SensorFusionProperties::VehicleX];
					double vy = mSensorFusionData[i][SensorFusionProperties::VehicleY];

					double check_speed = sqrt(vx*vx+vy*vy);

					// calculate lag corrected position of the car
					double futureCarS = mSensorFusionData[i][SensorFusionProperties::VehicleS] + ((double)miPreviousSize * mFrequency*check_speed);

					// is the car in front of us and within our safety distance? set costs the higher the closer the vehicle is
					if(futureCarS>mCarS && (futureCarS-mCarS)<safetyDistance)
					{
						double bbf = safetyDistance-(futureCarS-mCarS);
						if(blockedByCarInFront<bbf)
						{
							blockedByCarInFront = bbf;
						}
					}

					// check vehicles at direct side
					double futureDist = 2.0 * (check_speed - mCarSpeed);	// we need to take our speed vs other vehicle's speed always into account
					if(futureDist<0)
					{
						futureDist = 0.0;
					}

					// calculate "no-go" area which likely lead to a crash if blocked by a car
					double safetyRegionStart = mCarS-mSafetyBuffer;
					double safetyRegionEnd = mCarS+mSafetyBuffer;

					// if the car is directly in front of us or will be (because it's far faster) likely be in front of us very soon or right next
					// to our side... don't consider this lane
					if(futureCarS+futureDist>=safetyRegionStart && futureCarS<=safetyRegionEnd)
					{
						blockedByCarAtSide = true;
						break;		// no need for further tests
					}
				}
			}

			if(blockedByCarAtSide)
			{
				costs += mdCriticalCosts;	// don't use the lane if there is a car blocking it
			}
			costs += blockedByCarInFront; 	// if there is a car in front of any lane a lane switch should be considered

			laneCosts.push_back(costs);		// store overall lane costs
		}

		return laneCosts;
	}

	//! Returns the current lane index
	/** @return The current lane's index. 0 = most left lane .. miLaneCount-1 */
	int GetCurrentLane()
	{
		return mCarD/mLaneWidth;
	}

	//! Returns the offset (im meters) to the optimal lane center of the car's current lane
	/** @return The offset in meters */
	double GetLaneOffset()
	{
		int currentLane = GetCurrentLane();
		return mCarD-(currentLane*mLaneWidth+mHalfLaneWidth);
	}

	//! Chooses the target lane
	/** The target lane index */
	int ChooseTargetLane()
	{
		int targetLane = 0;
		auto costs = CalculateLaneCosts();
		double bestCosts = costs[0];
		for(int i=1; i<costs.size(); ++i)
		{
			if(costs[i]<bestCosts)
			{
				bestCosts = costs[i];
				targetLane = i;
			}
		}

		// don't change lances at all if situation is chaotic atm
		double maxCosts = mdMaxLaneChangeCosts;
		if(bestCosts>maxCosts)
		{
			targetLane = GetCurrentLane();
		}

		return targetLane;
	}

	//! Calculates the safety distance at the current speed
	double CalculateSafetyDistance()
	{
		double msp = mCarSpeed / 10;
		return msp*msp + msp*3;
	}

	//! Checks the distance to the vehicles in front of ourselfs. If the car is transitioning it also checks the distance to the other cars
	void CheckDistances()
	{
		double safetyDistance = CalculateSafetyDistance();

		int currentLane = GetCurrentLane();
		vector<int> lanes = {currentLane};
		double off = mCarD-(currentLane*mLaneWidth+mHalfLaneWidth);

		// if the car is transitioning to left or right also consider the lanes it already intersects
		if(off<-mHalfLaneWidth/2 && currentLane>0)
		{
			lanes.push_back(currentLane-1);
		}
		if(off>mHalfLaneWidth/2 && currentLane<miLaneCount-1)
		{
			lanes.push_back(currentLane+1);
		}

		bool brake = false; // braking not required by default

		// for all cars nearby...
		for(int i=0; i<mSensorFusionData.size(); ++i)
		{
			// for all potential lanes (may be two while transitioning)
			for(auto lane:lanes)
			{
				// is this car in this lane ?
				float d = mSensorFusionData[i][SensorFusionProperties::VehicleD];
				if(d < (mHalfLaneWidth+mLaneWidth*lane+mHalfLaneWidth) && d > (mHalfLaneWidth+mLaneWidth*lane-mHalfLaneWidth) )
				{
					double vx = mSensorFusionData[i][SensorFusionProperties::VehicleX];
					double vy = mSensorFusionData[i][SensorFusionProperties::VehicleY];
					double check_speed = sqrt(vx*vx+vy*vy);
					double futureCarS = mSensorFusionData[i][SensorFusionProperties::VehicleS];

					// calculate lag corrected position
					futureCarS += ((double)miPreviousSize * mFrequency *check_speed);

					// enable brakes if vehicle gets close
					if(futureCarS>mCarS && (futureCarS-mCarS)<safetyDistance)
					{
						brake = true;
					}
				}
			}
		}

		// accelerate if possible, brake otherwise
		if(brake)
		{
			mTargetSpeed -= mMaxBrake;
			if(mTargetSpeed<0.0)
			{
				mTargetSpeed = 0.0;
			}
		}
		else if(mTargetSpeed<mOptimalSpeed)
		{
			mTargetSpeed += mMaxAccelerate;
		}
	}

	//! Calculates the trajectory and how to behave in general in the current situation. Evaluates if a lane shift
	//! makes sense, brakes and accelerates as appropriate.
	void CalculateTrajectory(vector<double> &trajectoryCoordsX, vector<double> &trajectoryCoordsY)
	{
		// ---- Consider lane switches ----
		int currentLane = GetCurrentLane();
		double offset = GetLaneOffset();

		// Remember for how long we are in the left lane.
		// Left lanes are penalized and lane switch costs decreases as longer we are in one of the left lanes
		miLeftLanePenalty += (miLaneCount-1-currentLane);

		if(miTargetLane==-1 && mCarSpeed>=mOptimalSpeed*2/3) // if there is no lane change in progress and we are fast enough so it's safe
		{
			miTargetLane = ChooseTargetLane();

			if(miTargetLane>currentLane)	// reset left lane penalty when ever moving to a right lane
			{
				miLeftLanePenalty = 0;
			}
		}
		else
		{
			if(currentLane==miTargetLane && fabs(offset)<mMaxLaneOffset)
			{
				miTargetLane = -1;
			}
		}
		int targetLane = miTargetLane!=-1 ? miTargetLane : currentLane;

		// ---- Accelerate or brake as required ----
		CheckDistances();

		double referenceX = mCarX;
		double referenceY = mCarY;
		double referenceYaw = deg2rad(mCarYaw);

		// ---- Calculate anchor points for a spline computation. The first two points contain either the current
		// trajectories most recent points or a guessed line based upon the car's current yaw ---

		vector<double> 	anchorPointsX;
		vector<double> 	anchorPointsY;
		if(miPreviousSize<2) // Not enough data available? Construct points by backwards projecting the direction
		{
			double prev_car_x = mCarX - cos(mCarYaw);
			double prev_car_y = mCarY - sin(mCarYaw);

			anchorPointsX.push_back(prev_car_x);
			anchorPointsY.push_back(prev_car_y);

			anchorPointsX.push_back(mCarX);
			anchorPointsY.push_back(mCarY);
		}
		else // if real data is available use nodes of previous path
		{
			referenceX = mPreviousPathX[miPreviousSize-1];
			referenceY = mPreviousPathY[miPreviousSize-1];

			double ref_x_prev = mPreviousPathX[miPreviousSize-2];
			double ref_y_prev = mPreviousPathY[miPreviousSize-2];
			referenceYaw = atan2(referenceY-ref_y_prev, referenceX-ref_x_prev);

			anchorPointsX.push_back(ref_x_prev);
			anchorPointsY.push_back(ref_y_prev);

			anchorPointsX.push_back(referenceX);
			anchorPointsY.push_back(referenceY);
		}


		// --- Add three future points on the target lane ---
		for(int i=0; i<mBaseNodeCount; ++i)
		{
			vector<double> nextWayPoint = getXY(mCarS + (i+1) * mBaseNodeStepping, (mHalfLaneWidth+mLaneWidth*targetLane), *mWayPointsS, *mWayPointsX, *mWayPointsY);
			anchorPointsX.push_back(nextWayPoint[0]);
			anchorPointsY.push_back(nextWayPoint[1]);
		}

		// --- Transform anchor points to local coordinates ---
		for(int i=0; i<anchorPointsX.size(); ++i)
		{
			double relativeX =anchorPointsX[i]-referenceX;
			double relativeY =anchorPointsY[i]-referenceY;

			anchorPointsX[i] = (relativeX * cos(-referenceYaw)-relativeY*sin(-referenceYaw));
			anchorPointsY[i] = (relativeX * sin(-referenceYaw)+relativeY*cos(-referenceYaw));
		}

		// --- Calculate spline using the just defined anchor points ---
		tk::spline s;
		s.set_points(anchorPointsX, anchorPointsY);

		// --- Transfer the previous path's points so they will become the new nodes of the new trajectory ---
		for(int i=0; i<mPreviousPathX.size(); ++i)
		{
			trajectoryCoordsX.push_back(mPreviousPathX[i]);
			trajectoryCoordsY.push_back(mPreviousPathY[i]);
		}

		// --- Add all missing points (always looking 1 second into the future) to the current trajectory ---
		double targetS = s(mTargetOffset);
		double targetDistance = sqrt(mTargetOffset*mTargetOffset+targetS*targetS);
		double startX = 0;
		for(int i=1; i<= mPlanningPointCount - mPreviousPathX.size(); ++i)
		{
			double N = (targetDistance/(mFrequency*mTargetSpeed/2.24));
			double nextX = startX + (mTargetOffset)/N;
			double nextY = s(nextX);

			startX = nextX;

			auto orgNextX = nextX;
			auto orgNextY = nextY;

			nextX = (orgNextX * cos(referenceYaw)-orgNextY*sin(referenceYaw));
			nextY = (orgNextX * sin(referenceYaw)+orgNextY*cos(referenceYaw));

			nextX += referenceX;
			nextY += referenceY;

			trajectoryCoordsX.push_back(nextX);
			trajectoryCoordsY.push_back(nextY);
		}
	}

};

// ---------------------------------------------------------------------------------------------------------------------

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  	PathPlanner pathPlanner;

	h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy, &pathPlanner](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {

        	// extract data from json object
			double carX = j[1]["x"];
			double carY = j[1]["y"];
			double carS = j[1]["s"];
			double carD = j[1]["d"];
			double carYaw = j[1]["yaw"];
			double carSpeed = j[1]["speed"];
			vector<double> previousPathX = j[1]["previous_path_x"];
			vector<double> previousPathY = j[1]["previous_path_y"];
			double endPathS = j[1]["end_path_s"];
			double endPathD = j[1]["end_path_d"];
			vector<vector<double> >  sensorFusionData = j[1]["sensor_fusion"];

			// pass data to planner
			pathPlanner.SetFrameData(&map_waypoints_x, &map_waypoints_y, &map_waypoints_s, &map_waypoints_dx, &map_waypoints_dy,
					carX, carY, carS, carD, carYaw, carSpeed,
					endPathS, endPathD, previousPathX, previousPathY, sensorFusionData);

			vector<double> 	next_x_vals,
							next_y_vals;

			// calculate next trajectory
			pathPlanner.CalculateTrajectory(next_x_vals, next_y_vals);

			// forward trajectory to simulator
			json msgJson;

			// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
