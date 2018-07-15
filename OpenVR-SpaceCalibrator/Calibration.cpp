#include "stdafx.h"
#include "Calibration.h"
#include "Configuration.h"

#include <string>
#include <vector>
#include <iostream>

#include <lib_vrinputemulator/vrinputemulator.h>
#include <Eigen/Dense>


static vrinputemulator::VRInputEmulator InputEmulator;
CalibrationContext CalCtx;

void InitVR()
{
	auto initError = vr::VRInitError_None;
	vr::VR_Init(&initError, vr::VRApplication_Other);
	if (initError != vr::VRInitError_None)
	{
		auto error = vr::VR_GetVRInitErrorAsEnglishDescription(initError);
		throw std::runtime_error("OpenVR error:" + std::string(error));
	}

	if (!vr::VR_IsInterfaceVersionValid(vr::IVRSystem_Version))
	{
		throw std::runtime_error("OpenVR error: Outdated IVRSystem_Version");
	}
	else if (!vr::VR_IsInterfaceVersionValid(vr::IVRSettings_Version))
	{
		throw std::runtime_error("OpenVR error: Outdated IVRSettings_Version");
	}

	InputEmulator.connect();
}

struct Pose
{
	Eigen::Matrix3d rot;
	Eigen::Vector3d trans;

	Pose() { }
	Pose(vr::HmdMatrix34_t hmdMatrix)
	{
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				rot(i,j) = hmdMatrix.m[i][j];
			}
		}
		trans = Eigen::Vector3d(hmdMatrix.m[0][3], hmdMatrix.m[1][3], hmdMatrix.m[2][3]);
	}
	Pose(double x, double y, double z) : trans(Eigen::Vector3d(x,y,z)) { }
};

struct Sample
{
	Pose ref, target;
	bool valid;
	Sample() : valid(false) { }
	Sample(Pose ref, Pose target) : valid(true), ref(ref), target(target) { }
};

struct DSample
{
	bool valid;
	Eigen::Vector3d ref, target;
};

bool StartsWith(const std::string &str, const std::string &prefix)
{
	if (str.length() < prefix.length())
		return false;

	return str.compare(0, prefix.length(), prefix) == 0;
}

bool EndsWith(const std::string &str, const std::string &suffix)
{
	if (str.length() < suffix.length())
		return false;

	return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

Eigen::Vector3d AxisFromRotationMatrix3(Eigen::Matrix3d rot)
{
	return Eigen::Vector3d(rot(2,1) - rot(1,2), rot(0,2) - rot(2,0), rot(1,0) - rot(0,1));
}

double AngleFromRotationMatrix3(Eigen::Matrix3d rot)
{
	return acos((rot(0,0) + rot(1,1) + rot(2,2) - 1.0) / 2.0);
}

DSample DeltaRotationSamples(Sample s1, Sample s2)
{
	// Difference in rotation between samples.
	auto dref = s1.ref.rot * s2.ref.rot.transpose();
	auto dtarget = s1.target.rot * s2.target.rot.transpose();

	// When stuck together, the two tracked objects rotate as a pair,
	// therefore their axes of rotation must be equal between any given pair of samples.
	DSample ds;
	ds.ref = AxisFromRotationMatrix3(dref);
	ds.target = AxisFromRotationMatrix3(dtarget);

	// Reject samples that were too close to each other.
	auto refA = AngleFromRotationMatrix3(dref);
	auto targetA = AngleFromRotationMatrix3(dtarget);
	ds.valid = refA > 0.4 && targetA > 0.4 && ds.ref.norm() > 0.01 && ds.target.norm() > 0.01;

	ds.ref.normalize();
	ds.target.normalize();
	return ds;
}

Eigen::Vector3d CalibrateRotation(const std::vector<Sample> &samples)
{
	std::vector<DSample> deltas;

	for (size_t i = 0; i < samples.size(); i++)
	{
		for (size_t j = 0; j < i; j++)
		{
			auto delta = DeltaRotationSamples(samples[i], samples[j]);
			if (delta.valid)
				deltas.push_back(delta);
		}
	}
	char buf[256];
	snprintf(buf, sizeof buf, "Got %zd samples with %zd delta samples\n", samples.size(), deltas.size());
	CalCtx.Message(buf);

	// Kabsch algorithm

	Eigen::MatrixXd refPoints(deltas.size(), 3), targetPoints(deltas.size(), 3);
	Eigen::Vector3d refCentroid(0,0,0), targetCentroid(0,0,0);

	for (size_t i = 0; i < deltas.size(); i++)
	{
		refPoints.row(i) = deltas[i].ref;
		refCentroid += deltas[i].ref;

		targetPoints.row(i) = deltas[i].target;
		targetCentroid += deltas[i].target;
	}

	refCentroid /= (double) deltas.size();
	targetCentroid /= (double) deltas.size();

	for (size_t i = 0; i < deltas.size(); i++)
	{
		refPoints.row(i) -= refCentroid;
		targetPoints.row(i) -= targetCentroid;
	}

	auto crossCV = refPoints.transpose() * targetPoints;

	Eigen::BDCSVD<Eigen::MatrixXd> bdcsvd;
	auto svd = bdcsvd.compute(crossCV, Eigen::ComputeThinU | Eigen::ComputeThinV);

	Eigen::Matrix3d i = Eigen::Matrix3d::Identity();
	if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0)
	{
		i(2,2) = -1;
	}

	Eigen::Matrix3d rot = svd.matrixV() * i * svd.matrixU().transpose();
	rot.transposeInPlace();

	Eigen::Vector3d euler = rot.eulerAngles(2, 1, 0) * 180.0 / EIGEN_PI;

	snprintf(buf, sizeof buf, "Calibrated rotation: yaw=%.2f pitch=%.2f roll=%.2f\n", euler[1], euler[2], euler[0]);
	CalCtx.Message(buf);
	return euler;
}

Eigen::Vector3d CalibrateTranslation(const std::vector<Sample> &samples)
{
	std::vector<std::pair<Eigen::Vector3d, Eigen::Matrix3d>> deltas;

	for (size_t i = 0; i < samples.size(); i++)
	{
		for (size_t j = 0; j < i; j++)
		{
			auto QAi = samples[i].ref.rot.transpose();
			auto QAj = samples[j].ref.rot.transpose();
			auto dQA = QAj - QAi;
			auto CA = QAj * (samples[j].ref.trans - samples[j].target.trans) - QAi * (samples[i].ref.trans - samples[i].target.trans);
			deltas.push_back(std::make_pair(CA, dQA));

			auto QBi = samples[i].target.rot.transpose();
			auto QBj = samples[j].target.rot.transpose();
			auto dQB = QBj - QBi;
			auto CB = QBj * (samples[j].ref.trans - samples[j].target.trans) - QBi * (samples[i].ref.trans - samples[i].target.trans);
			deltas.push_back(std::make_pair(CB, dQB));
		}
	}

	Eigen::VectorXd constants(deltas.size() * 3);
	Eigen::MatrixXd coefficients(deltas.size() * 3, 3);

	for (size_t i = 0; i < deltas.size(); i++)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			constants(i * 3 + axis) = deltas[i].first(axis);
			coefficients.row(i * 3 + axis) = deltas[i].second.row(axis);
		}
	}

	Eigen::Vector3d trans = coefficients.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(constants);
	auto transcm = trans * 100.0;

	char buf[256];
	snprintf(buf, sizeof buf, "Calibrated translation x=%.2f y=%.2f z=%.2f\n", transcm[0], transcm[1], transcm[2]);
	CalCtx.Message(buf);
	return transcm;
}

Sample CollectSample(const CalibrationContext &ctx)
{
	vr::TrackedDevicePose_t reference, target;
	reference.bPoseIsValid = false;
	target.bPoseIsValid = false;

	reference = ctx.devicePoses[ctx.referenceID];
	target = ctx.devicePoses[ctx.targetID];

	bool ok = true;
	if (!reference.bPoseIsValid)
	{
		CalCtx.Message("Reference device is not tracking\n"); ok = false;
	}
	if (!target.bPoseIsValid)
	{
		CalCtx.Message("Target device is not tracking\n"); ok = false;
	}
	if (!ok)
	{
		CalCtx.Message("Aborting calibration!\n");
		CalCtx.state = CalibrationState::None;
		return Sample();
	}

	return Sample(
		Pose(reference.mDeviceToAbsoluteTracking),
		Pose(target.mDeviceToAbsoluteTracking)
	);
}

vr::HmdQuaternion_t VRRotationQuat(Eigen::Vector3d eulerdeg)
{
	auto euler = eulerdeg * EIGEN_PI / 180.0;

	Eigen::Quaterniond rotQuat =
		Eigen::AngleAxisd(euler(0), Eigen::Vector3d::UnitZ()) *
		Eigen::AngleAxisd(euler(1), Eigen::Vector3d::UnitY()) *
		Eigen::AngleAxisd(euler(2), Eigen::Vector3d::UnitX());

	vr::HmdQuaternion_t vrRotQuat;
	vrRotQuat.x = rotQuat.coeffs()[0];
	vrRotQuat.y = rotQuat.coeffs()[1];
	vrRotQuat.z = rotQuat.coeffs()[2];
	vrRotQuat.w = rotQuat.coeffs()[3];
	return vrRotQuat;
}

vr::HmdVector3d_t VRTranslationVec(Eigen::Vector3d transcm)
{
	auto trans = transcm * 0.01;
	vr::HmdVector3d_t vrTrans;
	vrTrans.v[0] = trans[0];
	vrTrans.v[1] = trans[1];
	vrTrans.v[2] = trans[2];
	return vrTrans;
}

void ScanAndApplyProfile(CalibrationContext &ctx)
{
	char buffer[vr::k_unMaxPropertyStringSize];

	for (uint32_t id = 0; id < vr::k_unMaxTrackedDeviceCount; ++id)
	{
		auto deviceClass = vr::VRSystem()->GetTrackedDeviceClass(id);
		if (deviceClass == vr::TrackedDeviceClass_Invalid)
			continue;

		/*if (deviceClass == vr::TrackedDeviceClass_HMD) // for debugging unexpected universe switches
		{
			vr::ETrackedPropertyError err = vr::TrackedProp_Success;
			auto universeId = vr::VRSystem()->GetUint64TrackedDeviceProperty(id, vr::Prop_CurrentUniverseId_Uint64, &err);
			printf("uid %d err %d\n", universeId, err);
			continue;
		}*/

		vr::ETrackedPropertyError err = vr::TrackedProp_Success;
		vr::VRSystem()->GetStringTrackedDeviceProperty(id, vr::Prop_TrackingSystemName_String, buffer, vr::k_unMaxPropertyStringSize, &err);

		if (err != vr::TrackedProp_Success)
			continue;

		std::string trackingSystem(buffer);

		if (trackingSystem != ctx.targetTrackingSystem)
			continue;

		if (deviceClass == vr::TrackedDeviceClass_TrackingReference || deviceClass == vr::TrackedDeviceClass_HMD)
		{
			// TODO(pushrax): detect zero reference switches and adjust calibration automatically
			//auto p = ctx.devicePoses[id].mDeviceToAbsoluteTracking.m;
			//printf("REF %d: %f %f %f\n", id, p[0][3], p[1][3], p[2][3]);
		}
		else
		{
			//printf("setting calibration for %d (%s)\n", id, buffer);
			auto vrRotQuat = VRRotationQuat(ctx.calibratedRotation);
			InputEmulator.setWorldFromDriverRotationOffset(id, vrRotQuat);

			auto vrTransVec = VRTranslationVec(ctx.calibratedTranslation);
			InputEmulator.setWorldFromDriverTranslationOffset(id, vrTransVec);

			InputEmulator.enableDeviceOffsets(id, true);
		}
	}
}

void ResetAndDisableOffsets(uint32_t id)
{
	vr::HmdVector3d_t zeroV;
	zeroV.v[0] = zeroV.v[1] = zeroV.v[2] = 0;

	vr::HmdQuaternion_t zeroQ;
	zeroQ.x = 0; zeroQ.y = 0; zeroQ.z = 0; zeroQ.w = 1;

	InputEmulator.setWorldFromDriverRotationOffset(id, zeroQ);
	InputEmulator.setWorldFromDriverTranslationOffset(id, zeroV);
	InputEmulator.enableDeviceOffsets(id, false);
}

void StartCalibration()
{
	CalCtx.state = CalibrationState::Begin;
	CalCtx.wantedUpdateInterval = 0.0;
	CalCtx.messages = "";
}

void CalibrationTick(double time)
{
	if (!vr::VRSystem())
		return;

	auto &ctx = CalCtx;
	if ((time - ctx.timeLastTick) < 0.05)
		return;

	ctx.timeLastTick = time;
	vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.0f, ctx.devicePoses, vr::k_unMaxTrackedDeviceCount);

	if (ctx.state == CalibrationState::None)
	{
		ctx.wantedUpdateInterval = 1.0;

		if (!ctx.validProfile)
			return;

		if ((time - ctx.timeLastScan) >= 2.5)
		{
			ScanAndApplyProfile(ctx);
			ctx.timeLastScan = time;
		}
		return;
	}

	if (ctx.state == CalibrationState::Editing)
	{
		ctx.wantedUpdateInterval = 0.0;

		if (!ctx.validProfile)
			return;

		ScanAndApplyProfile(ctx);
		return;
	}

	if (ctx.state == CalibrationState::Begin)
	{
		bool ok = true;

		if (ctx.referenceID == -1)
		{
			CalCtx.Message("Missing reference device\n"); ok = false;
		}
		else if (!ctx.devicePoses[ctx.referenceID].bPoseIsValid)
		{
			CalCtx.Message("Reference device is not tracking\n"); ok = false;
		}

		if (ctx.targetID == -1)
		{
			CalCtx.Message("Missing target device\n"); ok = false;
		}
		else if (!ctx.devicePoses[ctx.targetID].bPoseIsValid)
		{
			CalCtx.Message("Target device is not tracking\n"); ok = false;
		}

		if (!ok)
		{
			ctx.state = CalibrationState::None;
			CalCtx.Message("Aborting calibration!\n");
			return;
		}

		ResetAndDisableOffsets(ctx.targetID);
		ctx.state = CalibrationState::Rotation;
		ctx.wantedUpdateInterval = 0.0;

		char buf[256];
		snprintf(buf, sizeof buf, "Starting calibration, referenceID=%d targetID=%d\n", ctx.referenceID, ctx.targetID);
		CalCtx.Message(buf);
		return;
	}

	auto sample = CollectSample(ctx);
	if (!sample.valid)
	{
		return;
	}
	CalCtx.Message(".");

	const int totalSamples = 100;
	static std::vector<Sample> samples;
	samples.push_back(sample);

	if (samples.size() == totalSamples)
	{
		CalCtx.Message("\n");
		if (ctx.state == CalibrationState::Rotation)
		{
			ctx.calibratedRotation = CalibrateRotation(samples);

			auto vrRotQuat = VRRotationQuat(ctx.calibratedRotation);
			InputEmulator.setWorldFromDriverRotationOffset(ctx.targetID, vrRotQuat);
			InputEmulator.enableDeviceOffsets(ctx.targetID, true);

			ctx.state = CalibrationState::Translation;
		}
		else if (ctx.state == CalibrationState::Translation)
		{
			ctx.calibratedTranslation = CalibrateTranslation(samples);

			auto vrTrans = VRTranslationVec(ctx.calibratedTranslation);
			InputEmulator.setWorldFromDriverTranslationOffset(ctx.targetID, vrTrans);

			SaveProfile(ctx);
			CalCtx.Message("Finished calibration, profile saved\n");

			ctx.state = CalibrationState::None;
		}

		samples.clear();
	}
}
