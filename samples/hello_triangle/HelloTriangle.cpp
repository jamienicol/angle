//
// Copyright 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

//            Based on Hello_Triangle.c from
// Book:      OpenGL(R) ES 2.0 Programming Guide
// Authors:   Aaftab Munshi, Dan Ginsburg, Dave Shreiner
// ISBN-10:   0321502795
// ISBN-13:   9780321502797
// Publisher: Addison-Wesley Professional
// URLs:      http://safari.informit.com/9780321563835
//            http://www.opengles-book.com

#include "SampleApplication.h"
#include <vector>

const int tex_size          = 60;
const int unpack_row_length = 64;
const int bpp               = 4;

class HelloTriangleSample : public SampleApplication
{
  public:
    HelloTriangleSample(int argc, char **argv)
        : SampleApplication("HelloTriangle", argc, argv, 3, 0)
    {}

    bool initialize() override
    {
        std::vector<uint8_t> data(tex_size * unpack_row_length * bpp);
        for (int j = 0; j < tex_size; j++) {
            for (int i = 0; i < tex_size; i++) {
                int offset = (j * unpack_row_length + i) * bpp;
                data[offset + 0] = 255;
                data[offset + 1] = 0;
                data[offset + 2] = 0;
                data[offset + 3] = 255;
            }
        }

        glGenBuffers(1, &mPbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mPbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, tex_size * unpack_row_length * bpp, data.data(), GL_STATIC_DRAW);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);

        glGenTextures(1, &mTex);
        glBindTexture(GL_TEXTURE_2D, mTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_size, tex_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        glGenFramebuffers(1, &mFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, mFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTex, 0);

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

        return true;
    }

    void destroy() override {
        // glDeleteProgram(mProgram);
    }

    void draw() override
    {
        // Set the viewport
        glViewport(0, 0, getWindow()->getWidth(), getWindow()->getHeight());

        // Clear the color buffer
        glClear(GL_COLOR_BUFFER_BIT);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, mFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, tex_size, tex_size, 0, 0, tex_size, tex_size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

  private:
    GLuint mTex;
    GLuint mPbo;
    GLuint mFbo;
};

int main(int argc, char **argv)
{
    HelloTriangleSample app(argc, argv);
    return app.run();
}
