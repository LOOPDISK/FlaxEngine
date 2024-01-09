// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

#include "InverseKinematics.h"

void InverseKinematics::SolveAimIK(const Transform& node, const Vector3& target, Quaternion& outNodeCorrection)
{
    Vector3 toTarget = target - node.Translation;
    toTarget.Normalize();
    const Vector3 fromNode = Vector3::Forward;
    Quaternion::FindBetween(fromNode, toTarget, outNodeCorrection);
}







void InverseKinematics::SolveTwoBoneIK(Transform& rootNode, Transform& jointNode, Transform& targetNode, const Vector3& target, const Vector3& jointTarget, bool allowStretching, float maxStretchScale)
{
    Real lowerLimbLength = (targetNode.Translation - jointNode.Translation).Length();
    Real upperLimbLength = (jointNode.Translation - rootNode.Translation).Length();
    Vector3 jointPos = jointNode.Translation;

    //MZ EDIT
    Vector3 rootPos = rootNode.Translation;
    Vector3 poleTargetPos = jointTarget;
    //MZ EDIT END

    Vector3 desiredDelta = target - rootNode.Translation;
    Real desiredLength = desiredDelta.Length();
    Real limbLengthLimit = lowerLimbLength + upperLimbLength;

    //MZ EDIT
    Vector3 rootToJoint = jointPos - rootPos;
    Vector3 rootToPole = poleTargetPos - rootPos;
    Vector3 projectedPoleTarget = poleTargetPos - Vector3::Dot(rootToPole, rootToJoint.GetNormalized()) * rootToJoint.GetNormalized();
    Vector3 desiredUpDirection = (projectedPoleTarget - jointPos).GetNormalized();
    //MZ EDIT END

    Vector3 desiredDir;
    if (desiredLength < ZeroTolerance)
    {
        desiredLength = ZeroTolerance;
        desiredDir = Vector3(1, 0, 0);
    }
    else
    {
        desiredDir = desiredDelta.GetNormalized();
    }

    Vector3 jointTargetDelta = jointTarget - rootNode.Translation;
    const Real jointTargetLengthSqr = jointTargetDelta.LengthSquared();

    Vector3 jointPlaneNormal, jointBendDir;
    if (jointTargetLengthSqr < ZeroTolerance * ZeroTolerance)
    {
        jointBendDir = Vector3::Forward;
        jointPlaneNormal = Vector3::Up;
    }
    else
    {
        jointPlaneNormal = desiredDir ^ jointTargetDelta;
        if (jointPlaneNormal.LengthSquared() < ZeroTolerance * ZeroTolerance)
        {
            desiredDir.FindBestAxisVectors(jointPlaneNormal, jointBendDir);
        }
        else
        {
            jointPlaneNormal.Normalize();
            jointBendDir = jointTargetDelta - (jointTargetDelta | desiredDir) * desiredDir;
            jointBendDir.Normalize();
        }
    }

    if (allowStretching)
    {
        const Real initialStretchRatio = 1.0f;
        const Real range = maxStretchScale - initialStretchRatio;
        if (range > ZeroTolerance && limbLengthLimit > ZeroTolerance)
        {
            const Real reachRatio = desiredLength / limbLengthLimit;
            const Real scalingFactor = (maxStretchScale - 1.0f) * Math::Saturate((reachRatio - initialStretchRatio) / range);
            if (scalingFactor > ZeroTolerance)
            {
                lowerLimbLength *= 1.0f + scalingFactor;
                upperLimbLength *= 1.0f + scalingFactor;
                limbLengthLimit *= 1.0f + scalingFactor;
            }
        }
    }

    Vector3 resultEndPos = target;
    Vector3 resultJointPos = jointPos;

    if (desiredLength >= limbLengthLimit)
    {
        resultEndPos = rootNode.Translation + limbLengthLimit * desiredDir;
        resultJointPos = rootNode.Translation + upperLimbLength * desiredDir;
    }
    else
    {
        const Real twoAb = 2.0f * upperLimbLength * desiredLength;
        const Real cosAngle = twoAb > ZeroTolerance ? (upperLimbLength * upperLimbLength + desiredLength * desiredLength - lowerLimbLength * lowerLimbLength) / twoAb : 0.0f;
        const bool reverseUpperBone = cosAngle < 0.0f;
        const Real angle = Math::Acos(cosAngle);
        const Real jointLineDist = upperLimbLength * Math::Sin(angle);
        const Real projJointDistSqr = upperLimbLength * upperLimbLength - jointLineDist * jointLineDist;
        Real projJointDist = projJointDistSqr > 0.0f ? Math::Sqrt(projJointDistSqr) : 0.0f;
        if (reverseUpperBone)
            projJointDist *= -1.0f;
        resultJointPos = rootNode.Translation + projJointDist * desiredDir + jointLineDist * jointBendDir;
    }

    // Calculate the rotation for the root bone
    {
        Vector3 rootToJoint = resultJointPos - rootPos;
        Vector3 rootToTarget = resultEndPos - rootPos;
        Vector3 planeNormal = Vector3::Cross(rootToJoint, rootToTarget).GetNormalized();
        Vector3 desiredYAxis = (jointTarget - rootPos).GetNormalized();
        Vector3 xAxis = Vector3::Cross(desiredYAxis, planeNormal).GetNormalized();

        Matrix rootRotationMatrix;
        rootRotationMatrix.SetColumn1(Vector4(xAxis, 0));
        rootRotationMatrix.SetColumn2(Vector4(desiredYAxis, 0));
        rootRotationMatrix.SetColumn3(Vector4(planeNormal, 0));
        rootRotationMatrix.SetColumn4(Vector4(rootPos, 1));

        rootNode.SetRotation(rootRotationMatrix);
    }

    // Calculate the rotation for the joint bone
    {
        Vector3 jointToTarget = resultEndPos - resultJointPos;
        Vector3 planeNormal = Vector3::Cross(jointToTarget, (jointTarget - resultJointPos)).GetNormalized();
        Vector3 desiredYAxis = (jointTarget - resultJointPos).GetNormalized();
        Vector3 xAxis = Vector3::Cross(desiredYAxis, planeNormal).GetNormalized();

        Matrix jointRotationMatrix;
        jointRotationMatrix.SetColumn1(Vector4(xAxis, 0));
        jointRotationMatrix.SetColumn2(Vector4(desiredYAxis, 0));
        jointRotationMatrix.SetColumn3(Vector4(planeNormal, 0));
        jointRotationMatrix.SetColumn4(Vector4(resultJointPos, 1));

        jointNode.SetRotation(jointRotationMatrix);
    }

    jointNode.Translation = resultJointPos;

    targetNode.Translation = resultEndPos;
}
