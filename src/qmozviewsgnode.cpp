/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "qmozviewsgnode.h"

#include <QtGui/QPolygonF>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleRectNode>
#include <private/qsgrendernode_p.h>
#include "qgraphicsmozview_p.h"
#include <assert.h>

class MozContentSGNode : public QSGRenderNode {
public:
    MozContentSGNode(QGraphicsMozViewPrivate* aPrivate)
        : mPrivate(aPrivate)
    {
        mPrivate->mView->SetIsActive(true);
    }

    virtual StateFlags changedStates()
    {
        return StateFlags(StencilState) | ColorState | BlendState;
    }

    virtual void render(const RenderState& state)
    {
        QMatrix affine = matrix() ? (*matrix()).toAffine() : QMatrix();
        gfxMatrix matr(affine.m11(), affine.m12(), affine.m21(), affine.m22(), affine.dx(), affine.dy());
        mPrivate->mView->SetGLViewTransform(matr);
        mPrivate->mView->RenderGL();
    }

    ~MozContentSGNode()
    {
    }

    const QMozViewSGNode* pageNode() const
    {
        const QMozViewSGNode* parent = static_cast<QMozViewSGNode*>(this->parent());
        assert(parent);
        return parent;
    }

    QGraphicsMozViewPrivate* getPrivate() { return mPrivate; }

private:
    QGraphicsMozViewPrivate* mPrivate;
};

QMozViewSGNode::QMozViewSGNode()
    : m_contentsNode(0)
{
}

void QMozViewSGNode::setRenderer(QGraphicsMozViewPrivate* aPrivate)
{
    if (m_contentsNode && m_contentsNode->getPrivate() == aPrivate)
        return;

    if (m_contentsNode) {
        removeChildNode(m_contentsNode);
        delete m_contentsNode;
    }
    m_contentsNode = new MozContentSGNode(aPrivate);
    // This sets the parent node of the content to QMozViewSGNode.
    appendChildNode(m_contentsNode);
}