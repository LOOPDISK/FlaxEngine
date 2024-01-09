// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

#include "InverseKinematics.h"

void InverseKinematics::SolveAimIK(const Transform& node, const Vector3& target, Quaternion& outNodeCorrection)
{
    Vector3 toTarget = target - node.Translation;
    toTarget.Normalize();
    const Vector3 fromNode = Vector3::Forward;
    Quaternion::FindBetween(fromNode, toTarget, outNodeCorrection);
}


Vector3 InverseKinematics::ProjectOntoPlane(const Vector3& vector, const Vector3& planeNormal)
{
    return vector - Vector3::Dot(vector, planeNormal) * planeNormal;
}

float InverseKinematics::CalculateAngleBetweenVectors(const Vector3& vec1, const Vector3& vec2, const Vector3& normal)
{
    Vector3 crossProduct = Vector3::Cross(vec1, vec2);
    float dotProduct = Vector3::Dot(vec1, vec2);
    float angle = static_cast<float>(atan2(crossProduct.Length(), dotProduct));
    return Vector3::Dot(crossProduct, normal) < 0.0f ? -angle : angle;
}



void InverseKinematics::ApplyTwistRotation(Transform& bone, const Vector3& rootPosition, const Vector3& jointPosition, const Vector3& targetPosition, const Vector3& poleTarget)
{
    // Calculate the plane normal using the three IK points
    Vector3 rootToJoint = jointPosition - rootPosition;
    Vector3 rootToTarget = targetPosition - rootPosition;
    Vector3 planeNormal = Vector3::Cross(rootToJoint, rootToTarget).GetNormalized();

    // Align the bone's Y-axis towards the desired up direction (pole target direction)
    Vector3 desiredUp = poleTarget - jointPosition;
    Vector3 desiredUpProjected = ProjectOntoPlane(desiredUp, planeNormal);
    Quaternion upRotation;
    Quaternion::RotationAxis(rootToJoint.GetNormalized(), CalculateAngleBetweenVectors(bone.Orientation * Vector3::UnitY, desiredUpProjected, rootToJoint), upRotation);
    Quaternion newOrientation = upRotation * bone.Orientation;

    // Adjust the bone's Z-axis to be perpendicular to the plane
    Vector3 newForward = Vector3::Cross(planeNormal, newOrientation * Vector3::UnitY).GetNormalized();
    Quaternion forwardRotation;
    Quaternion::RotationAxis(newOrientation * Vector3::UnitY, CalculateAngleBetweenVectors(newOrientation * Vector3::Forward, newForward, newOrientation * Vector3::UnitY), forwardRotation);
    newOrientation = forwardRotation * newOrientation;

    // Set the new orientation to the bone
    bone.Orientation = newOrientation;
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

    // Apply twist rotation to root bone
    ApplyTwistRotation(rootNode, rootNode.Translation, jointNode.Translation, targetNode.Translation, jointTarget);

    // Apply twist rotation to joint bone
    ApplyTwistRotation(jointNode, rootNode.Translation, jointNode.Translation, targetNode.Translation, jointTarget);


    {
        const Vector3 oldDir = (jointPos - rootNode.Translation).GetNormalized();
        const Vector3 newDir = (resultJointPos - rootNode.Translation).GetNormalized();
        const Quaternion deltaRotation = Quaternion::FindBetween(oldDir, newDir);
        rootNode.Orientation = deltaRotation * rootNode.Orientation;
    }

    {
        const Vector3 oldDir = (targetNode.Translation - jointPos).GetNormalized();
        const Vector3 newDir = (resultEndPos - resultJointPos).GetNormalized();
        const Quaternion deltaRotation = Quaternion::FindBetween(oldDir, newDir);
        jointNode.Orientation = deltaRotation * jointNode.Orientation;
        jointNode.Translation = resultJointPos;
    }




    targetNode.Translation = resultEndPos;
}
