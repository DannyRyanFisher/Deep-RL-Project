/* 
 * Author - Dustin Franklin (Nvidia Jetson Developer)
 * Modified by - Sahil Juneja, Kyle Stewart-Frantz
 *
 */

#include "ArmPlugin.h"
#include "PropPlugin.h"
#include "cudaMappedMemory.h"
#include "cudaPlanar.h"

#define PI 3.141592653589793238462643383279502884197169f

// Decide Positon or Velocity Control to be used
#define VELOCITY_CONTROL	false

// Position based control
#define JOINT_MIN		-0.75f
#define JOINT_MAX		2.0f

// Velocity based control
#define VELOCITY_MIN 	-0.2f
#define VELOCITY_MAX  	0.2f

// Define DQN API Settings

#define INPUT_CHANNELS 	3
#define ALLOW_RANDOM 	true
#define DEBUG_DQN 		false
#define GAMMA 			0.9f
#define EPS_START 		0.9f
#define EPS_END 		0.05f
#define EPS_DECAY 		200

// TODO - Tune the following hyperparameters

#define INPUT_WIDTH   	64			// Size of input will impact memory use
#define INPUT_HEIGHT  	64
#define NUM_ACTIONS		DOF*2		// Each joint action is either decreas or increas. DOF is number of joints
#define OPTIMIZER 		"RMSprop"	// RMSprop, Adam, AdaGrad, None.
#define LEARNING_RATE 	0.012f		// Smaller number will slow learning but better minimize error 
#define REPLAY_MEMORY 	11000
#define BATCH_SIZE 		32			// bigger size will require more computing power.
#define USE_LSTM 		true		
#define LSTM_SIZE 		256


// TODO - Define Reward Parameters

#define REWARD_WIN  	300.0f
#define REWARD_LOSS 	-300.0f
#define REWARD_MULT 	100.0f
#define alpha 			0.3f

// Define Object Names
#define WORLD_NAME 		"arm_world"
#define PROP_NAME  		"tube"
#define GRIP_NAME  		"gripper_middle"

// Define Collision Parameters
#define COLLISION_FILTER "ground_plane::link::collision"		// To detect ground collision
#define COLLISION_ITEM   "tube::tube_link::tube_collision"		// Object of interest for collision in both Tasks 1 & 2

#define COLLISION_POINT  "arm::gripperbase::gripper_link"		// Used for Task 2

// Animation Steps
#define ANIMATION_STEPS 1000

// Set Debug Mode
#define DEBUG 		false

// Lock base rotation DOF (Add dof in header file if off)
#define LOCKBASE 	true


namespace gazebo
{
 
	// register this plugin with the simulator
	GZ_REGISTER_MODEL_PLUGIN(ArmPlugin)

	//*********************************************************************************************************************************************************
	// constructor
	//*********************************************************************************************************************************************************
	ArmPlugin::ArmPlugin() : ModelPlugin(), cameraNode(new gazebo::transport::Node()), collisionNode(new gazebo::transport::Node())
	{
		if (DEBUG){printf("[DEBUG] ArmPlugin::ArmPlugin() Constructor\n");}

		agent 	       	 = NULL;
		inputState       = NULL;
		inputBuffer[0]   = NULL;
		inputBuffer[1]   = NULL;
		inputBufferSize  = 0;
		inputRawWidth    = 0;
		inputRawHeight   = 0;
		actionJointDelta = 0.15f;
		actionVelDelta   = 0.1f;
		maxEpisodeLength = 100;
		episodeFrames    = 0;

		newState         = false;
		newReward        = false;
		endEpisode       = false;
		rewardHistory    = 0.0f;
		testAnimation    = true;
		loopAnimation    = false;
		animationStep    = 0;
		lastGoalDistance = 0.0f;
		avgGoalDelta     = 0.0f;
		successfulGrabs = 0;
		totalRuns       = 0;

		// set the default reset position for all joint
		for( uint32_t n=0; n < DOF; n++ )
			resetPos[n] = 0.0f;

		// set the arm forward
		resetPos[1] = 1.0f;

		// set the initial positions and velocities
		for( uint32_t n=0; n < DOF; n++ )
		{
			ref[n] = resetPos[n]; //JOINT_MIN;
			vel[n] = 0.0f;
		}
	}

	//*********************************************************************************************************************************************************
	// Load
	//*********************************************************************************************************************************************************
	void ArmPlugin::Load(physics::ModelPtr _parent, sdf::ElementPtr /*_sdf*/) 
	{
		if (DEBUG) {printf("[DEBUG] ArmPlugin::Load('%s')\n", _parent->GetName().c_str());}

		// Create DQN agent
		if( !createAgent() ){
			printf("Error creatings DQN Agent\n");
			return;
		}

		// Store the pointer to the model
		this->model = _parent;
		this->j2_controller = new physics::JointController(model);

		// Create our node for camera communication
		cameraNode->Init();
		// TODO - Subscribe to camera topic
		cameraSub = cameraNode->Subscribe("/gazebo/arm_world/camera/link/camera/image", &ArmPlugin::onCameraMsg, this);

		// Create our node for collision detection
		collisionNode->Init();
		// TODO - Subscribe to prop collision topic
		collisionSub = collisionNode->Subscribe("/gazebo/arm_world/tube/tube_link/my_contact", &ArmPlugin::onCollisionMsg, this);

		// Listen to the update event. This event is broadcast every simulation iteration.
		this->updateConnection = event::Events::ConnectWorldUpdateBegin(boost::bind(&ArmPlugin::OnUpdate, this, _1));
	}

	//*********************************************************************************************************************************************************
	// CreateAgent
	//*********************************************************************************************************************************************************
	bool ArmPlugin::createAgent()
	{
		if( agent != NULL )
			return true;
				
		// TODO - Create DQN Agent
		agent = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, NUM_ACTIONS, 
									OPTIMIZER, LEARNING_RATE, REPLAY_MEMORY, BATCH_SIZE, 
									GAMMA, EPS_START, EPS_END, EPS_DECAY, 
									USE_LSTM, LSTM_SIZE, ALLOW_RANDOM, DEBUG_DQN);

		if( !agent )
		{
			printf("ArmPlugin - failed to create DQN agent\n");
			return false;
		}
		else
		{
			if (DEBUG) {printf("[DEBUG] ArmPlugin - created DQN agent successfuly\n");}
		}

		// Allocate the python tensor for passing the camera state
		inputState = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);

		if( !inputState )
		{
			printf("ArmPlugin - failed to allocate %ux%ux%u Tensor\n", INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
			return false;
		}

		return true;
	}

	//*********************************************************************************************************************************************************
	// onCameraMsg
	//*********************************************************************************************************************************************************
	void ArmPlugin::onCameraMsg(ConstImageStampedPtr &_msg)
	{
		// don't process the image if the agent hasn't been created yet
		if( !agent ) return;

		// check the validity of the message contents
		if( !_msg )
		{
			printf("ArmPlugin - recieved NULL message\n");
			return;
		}

		// retrieve image dimensions
		const int width  = _msg->image().width();
		const int height = _msg->image().height();
		const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
		const int size   = _msg->image().data().size();

		if( bpp != 24 )
		{
			printf("ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
			return;
		}

		// allocate temp image if necessary
		if( !inputBuffer[0] || size != inputBufferSize )
		{
			if( !cudaAllocMapped(&inputBuffer[0], &inputBuffer[1], size) )
			{
				printf("ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
				return;
			}

			printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);
			
			inputBufferSize = size;
			inputRawWidth   = width;
			inputRawHeight  = height;
		}

		memcpy(inputBuffer[0], _msg->image().data().c_str(), inputBufferSize);
		newState = true;

		if(DEBUG){printf("[DEBUG] camera %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);}

	}

	//*********************************************************************************************************************************************************
	// onCollisionMsg
	//*********************************************************************************************************************************************************
	void ArmPlugin::onCollisionMsg(ConstContactsPtr &contacts)
	{
		if(DEBUG){printf("[DEBUG] collision callback (%u contacts)\n", contacts->contact_size());}

		if( testAnimation )
			return;

		for (unsigned int i = 0; i < contacts->contact_size(); ++i)
		{
			// exit if collision is with ground
			if( strcmp(contacts->contact(i).collision2().c_str(), COLLISION_FILTER) == 0 )
				continue;

			if(DEBUG){std::cout << "Collision between[" << contacts->contact(i).collision1() << "] and ["
				                                        << contacts->contact(i).collision2() << "]\n";}

			// TODO - Check if there is collision between the arm and object, then issue learning reward
			if ((strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM  ) == 0))
				// && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT) == 0))  // add this to the "if" statment for Task 2 only
			{
				// collision reward
				rewardHistory = REWARD_WIN;
				newReward  = true;
				endEpisode = true;
			}
			else
			{
				// no collision with required target penalty
				rewardHistory = REWARD_LOSS * 0.1f;
				newReward  = true;
				endEpisode = false;
			}
			return;
		}
	}

	//*********************************************************************************************************************************************************
	// upon recieving a new frame, update the AI agent
	//*********************************************************************************************************************************************************
	bool ArmPlugin::updateAgent()
	{
		// convert uchar3 input from camera to planar BGR
		if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer[1], inputRawWidth, inputRawHeight,
										inputState->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
		{
			printf("ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
				inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

			return false;
		}

		// select the next action
		int action = 0;

		if( !agent->NextAction(inputState, &action) )
		{
			printf("ArmPlugin - failed to generate agent's next action\n");
			return false;
		}

		// make sure the selected action is in-bounds
		if( action < 0 || action >= DOF * 2 )
		{
			printf("ArmPlugin - agent selected invalid action, %i\n", action);
			return false;
		}

		if(DEBUG){printf("[DEBUG] ArmPlugin - agent selected action %i\n", action);}



		#if VELOCITY_CONTROL
			// if the action is even, increase the joint position by the delta parameter
			// if the action is odd,  decrease the joint position by the delta parameter

				
			/*
			/ TODO - Increase or decrease the joint velocity based on whether the action is even or odd
			/
			*/
			
			float velocity = 0.0; // TODO - Set joint velocity based on whether action is even or odd.

			if( velocity < VELOCITY_MIN )
				velocity = VELOCITY_MIN;

			if( velocity > VELOCITY_MAX )
				velocity = VELOCITY_MAX;

			vel[action/2] = velocity;
			
			for( uint32_t n=0; n < DOF; n++ )
			{
				ref[n] += vel[n];

				if( ref[n] < JOINT_MIN )
				{
					ref[n] = JOINT_MIN;
					vel[n] = 0.0f;
				}
				else if( ref[n] > JOINT_MAX )
				{
					ref[n] = JOINT_MAX;
					vel[n] = 0.0f;
				}
			}
		#else
			// TODO - Increase or decrease the joint position based on whether the action is even or odd
			// TODO - Set joint position based on whether action is even or odd.
			
			const int i = action / 2;
			float joint = ref[i] + actionJointDelta * ((action % 2 == 0) ? 1.0f : -1.0f); 

			// limit the joint to the specified range
			if( joint < JOINT_MIN )
				joint = JOINT_MIN;
			
			if( joint > JOINT_MAX )
				joint = JOINT_MAX;

			ref[i] = joint;

		#endif

		return true;
	}

	//*********************************************************************************************************************************************************
	// update joint reference positions, returns true if positions have been modified
	//*********************************************************************************************************************************************************
	bool ArmPlugin::updateJoints()
	{
		if( testAnimation )	// test sequence
		{
			const float step = (JOINT_MAX - JOINT_MIN) * (float(1.0f) / float(ANIMATION_STEPS));

			#if 0
					// range of motion
					if( animationStep < ANIMATION_STEPS )
					{
						animationStep++;
						printf("animation step %u\n", animationStep);

						for( uint32_t n=0; n < DOF; n++ )
							ref[n] = JOINT_MIN + step * float(animationStep);
					}
					else if( animationStep < ANIMATION_STEPS * 2 )
					{			
						animationStep++;
						printf("animation step %u\n", animationStep);

						for( uint32_t n=0; n < DOF; n++ )
							ref[n] = JOINT_MAX - step * float(animationStep-ANIMATION_STEPS);
					}
					else
					{
						animationStep = 0;

					}

			#else
					// return to base position
					for( uint32_t n=0; n < DOF; n++ )
					{
						
						if( ref[n] < resetPos[n] )
							ref[n] += step;
						else if( ref[n] > resetPos[n] )
							ref[n] -= step;

						if( ref[n] < JOINT_MIN )
							ref[n] = JOINT_MIN;
						else if( ref[n] > JOINT_MAX )
							ref[n] = JOINT_MAX;
						
					}

					animationStep++;
			#endif

			// reset and loop the animation
			if( animationStep > ANIMATION_STEPS )
			{
				animationStep = 0;
				
				if( !loopAnimation )
					testAnimation = false;
			}
			else if( animationStep == ANIMATION_STEPS / 2 )
			{	
				ResetPropDynamics();
			}

			return true;
		}

		else if( newState && agent != NULL )
		{
			// update the AI agent when new camera frame is ready
			episodeFrames++;

			if(DEBUG){printf("[DEBUG] episode frame = %i\n", episodeFrames);}

			// reset camera ready flag
			newState = false;

			if( updateAgent() )
				return true;
		}

		return false;
	}

	//*********************************************************************************************************************************************************
	// get the servo center for a particular degree of freedom
	//*********************************************************************************************************************************************************
	float ArmPlugin::resetPosition( uint32_t dof )
	{
		return resetPos[dof];
	}

	//*********************************************************************************************************************************************************
	// compute the distance between two bounding boxes
	//*********************************************************************************************************************************************************
	static float BoxDistance(const math::Box& a, const math::Box& b)
	{
		float sqrDist = 0;

		if( b.max.x < a.min.x )
		{
			float d = b.max.x - a.min.x;
			sqrDist += d * d;
		}
		else if( b.min.x > a.max.x )
		{
			float d = b.min.x - a.max.x;
			sqrDist += d * d;
		}

		if( b.max.y < a.min.y )
		{
			float d = b.max.y - a.min.y;
			sqrDist += d * d;
		}
		else if( b.min.y > a.max.y )
		{
			float d = b.min.y - a.max.y;
			sqrDist += d * d;
		}

		if( b.max.z < a.min.z )
		{
			float d = b.max.z - a.min.z;
			sqrDist += d * d;
		}
		else if( b.min.z > a.max.z )
		{
			float d = b.min.z - a.max.z;
			sqrDist += d * d;
		}
		
		return sqrtf(sqrDist);
	}

	//*********************************************************************************************************************************************************
	// called by the world update start event
	//*********************************************************************************************************************************************************
	void ArmPlugin::OnUpdate(const common::UpdateInfo& updateInfo)
	{
		// deferred loading of the agent (this is to prevent Gazebo black/frozen display)
		if( !agent && updateInfo.simTime.Float() > 1.5f )
		{
			if( !createAgent() )
				return;
		}

		// verify that the agent is loaded
		if( !agent )
			return;

		// determine if we have new camera state and need to update the agent
		const bool hadNewState = newState && !testAnimation;

		// update the robot positions with vision/DQN
		if( updateJoints() )
		{
			double angle(1);

			#if LOCKBASE
				j2_controller->SetJointPosition(this->model->GetJoint("base"), 		0);
				j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  	ref[0]);
				j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  	ref[1]);

			#else
				j2_controller->SetJointPosition(this->model->GetJoint("base"), 	 ref[0]); 
				j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[1]);
				j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[2]);
			#endif
		}

		// episode timeout
		if( maxEpisodeLength > 0 && episodeFrames > maxEpisodeLength )
		{
			printf("ArmPlugin - triggering EOE, episode has exceeded %i frames\n", maxEpisodeLength);
			rewardHistory = REWARD_LOSS;
			newReward     = true;
			endEpisode    = true;
		}

		// if an EOE reward hasn't already been issued, compute an intermediary reward
		if( hadNewState && !newReward )
		{
			PropPlugin* prop = GetPropByName(PROP_NAME);

			if( !prop )
			{
				printf("ArmPlugin - failed to find Prop '%s'\n", PROP_NAME);
				return;
			}

			// get the bounding box for the prop object
			const math::Box& propBBox = prop->model->GetBoundingBox();

			// get gripper link name
			physics::LinkPtr gripper  = model->GetLink(GRIP_NAME);
		
			if( !gripper )
			{
				printf("ArmPlugin - failed to find Gripper '%s'\n", GRIP_NAME);
				return;
			}




			// get the bounding box for the gripper		
			const math::Box& gripBBox = gripper->GetBoundingBox();

			// Ground Threshold
			const float groundContact = 0.05f;
			
			
			// TODO - set appropriate Reward for robot hitting the ground.
			if( gripBBox.min.z <= groundContact || gripBBox.max.z <= groundContact )
			{
				rewardHistory = REWARD_LOSS;
				newReward     = true;
				endEpisode    = true;
			}

			// TODO - Issue an interim reward based on the distance to the object		
			else
			{
				// calculate the distance between two bounding boxes
				const float distGoal = BoxDistance(gripBBox, propBBox);

				if( episodeFrames > 1 )
				{
					const float distDelta  	= lastGoalDistance - distGoal;
					avgGoalDelta  			= (avgGoalDelta * alpha) + (distDelta * (1.0 - alpha));
					rewardHistory 			= (avgGoalDelta) * REWARD_MULT;
					if (DEBUG) printf("Interim Reward %f \n", rewardHistory);
					newReward     		= true;	
				}

				lastGoalDistance = distGoal;
			}
		}

		// issue rewards and train DQN
		if( newReward && agent != NULL )
		{
			if(DEBUG){printf("[DEBUG] ArmPlugin - issuing reward %f, EOE=%s  %s\n", rewardHistory, endEpisode ? "true" : "false", (rewardHistory > 0.1f) ? "POS+" :(rewardHistory > 0.0f) ? "POS" : (rewardHistory < 0.0f) ? "    NEG" : "       ZERO");}
			
			agent->NextReward(rewardHistory, endEpisode);

			// reset reward indicator
			newReward = false;

			// reset for next episode
			if( endEpisode )
			{
				testAnimation    = true;	// reset the robot to base position
				loopAnimation    = false;
				endEpisode       = false;
				episodeFrames    = 0;
				lastGoalDistance = 0.0f;
				avgGoalDelta     = 0.0f;

				// track the number of wins and agent accuracy
				if( rewardHistory >= REWARD_WIN )
					successfulGrabs++;

				totalRuns++;
				printf("Current Accuracy:  %0.4f (%03u of %03u)  (reward=%+0.2f %s)\n", float(successfulGrabs)/float(totalRuns), successfulGrabs, totalRuns, rewardHistory, (rewardHistory >= REWARD_WIN ? "WIN" : "LOSS"));


				for( uint32_t n=0; n < DOF; n++ ) 
				{
					ref[n] = 0.0f;
					vel[n] = 0.0f;
				}
			}
		}
	}

}

